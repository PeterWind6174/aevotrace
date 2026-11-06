#include "pe.hpp"

#include "sha256.hpp"

#include <capstone/x86.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace aev::pe {
namespace {

constexpr uint16_t kMachineAmd64 = 0x8664;
constexpr uint16_t kMagicPe32Plus = 0x20b;
constexpr uint16_t kFileDll = 0x2000;
constexpr uint16_t kFileRelocsStripped = 0x0001;
constexpr uint16_t kFile32BitMachine = 0x0100;
constexpr uint16_t kDllGuardCf = 0x4000;
constexpr uint32_t kScnMemExecute = 0x20000000;
constexpr uint32_t kScnMemRead = 0x40000000;
constexpr uint32_t kScnMemWrite = 0x80000000;
constexpr uint32_t kDirExport = 0;
constexpr uint32_t kDirImport = 1;
constexpr uint32_t kDirException = 3;
constexpr uint32_t kDirBaseReloc = 5;
constexpr uint32_t kDirTls = 9;
constexpr uint32_t kDirLoadConfig = 10;
constexpr uint32_t kDirBoundImport = 11;
constexpr uint32_t kDirDelayImport = 13;

uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (alignment == 0)
        return value;
    return (value + alignment - 1) & ~(alignment - 1);
}
bool fits(uint64_t offset, uint64_t len, uint64_t total) {
    return offset <= total && len <= total - offset;
}
std::string hex(uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)value);
    return buf;
}
std::string hex32(uint32_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%x", value);
    return buf;
}
std::string short_name(const uint8_t *p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(reinterpret_cast<const char *>(p), len);
}
uint64_t fnv1a(const std::vector<uint8_t> &data) {
    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
void append_u32(std::vector<uint8_t> *out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out->push_back((uint8_t)((v >> (i * 8)) & 0xff));
}
void append_u64(std::vector<uint8_t> *out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out->push_back((uint8_t)((v >> (i * 8)) & 0xff));
}
void append_bytes(std::vector<uint8_t> *out, const std::string &s) {
    append_u32(out, (uint32_t)s.size());
    out->insert(out->end(), s.begin(), s.end());
}
std::string trim(const std::string &s) {
    size_t begin = 0, end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' ||
                           s[begin] == '\r' || s[begin] == '\n'))
        ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r' || s[end - 1] == '\n'))
        --end;
    return s.substr(begin, end - begin);
}

struct Parser {
    const Module *module = nullptr;
    std::string *error = nullptr;
    bool fail(const std::string &message) {
        if (error) *error = message;
        return false;
    }
};

}  // namespace

bool rva_to_file(const Module &module, uint32_t rva, uint32_t len,
                 uint32_t *offset, std::string *error) {
    if (len == 0) {
        if (error) *error = "zero-length RVA range";
        return false;
    }
    if (rva < module.size_of_headers) {
        if (!fits(rva, len, module.file_size)) {
            if (error) *error = "RVA in headers outside file: " + hex32(rva);
            return false;
        }
        if (offset) *offset = rva;
        return true;
    }
    for (const auto &section : module.sections) {
        uint64_t begin = section.virtual_address;
        uint64_t end = begin + section.raw_size;
        if (rva < begin || (uint64_t)rva >= end)
            continue;
        uint64_t in_section = rva - begin;
        if (!fits(in_section, len, section.raw_size)) {
            if (error) *error = "RVA crosses raw section data: " + hex32(rva);
            return false;
        }
        uint64_t file_off = (uint64_t)section.raw_ptr + in_section;
        if (!fits(file_off, len, module.file_size)) {
            if (error) *error = "RVA maps outside file: " + hex32(rva);
            return false;
        }
        if (offset) *offset = (uint32_t)file_off;
        return true;
    }
    if (error) *error = "RVA is not mapped by raw section data: " + hex32(rva);
    return false;
}

namespace {

bool read_cstring(const Module &module, uint32_t rva, std::string *out,
                  std::string *error) {
    uint32_t offset = 0;
    if (!rva_to_file(module, rva, 1, &offset, error))
        return false;
    std::string value;
    for (uint64_t i = offset; i < module.file.size() && value.size() <= kMaxName; ++i) {
        char c = (char)module.file[(size_t)i];
        if (c == 0) {
            *out = value;
            return true;
        }
        value.push_back(c);
    }
    if (error) *error = "unterminated/oversized string at RVA " + hex32(rva);
    return false;
}

bool parse_sections(Module *module, uint32_t section_table_offset,
                    uint16_t section_count, std::string *error) {
    if (section_count == 0 || section_count > kMaxSections) {
        if (error) *error = "invalid section count";
        return false;
    }
    uint64_t table_size = (uint64_t)section_count * 40;
    if (!fits(section_table_offset, table_size, module->file_size)) {
        if (error) *error = "section table outside file";
        return false;
    }
    module->sections.resize(section_count);
    for (uint16_t i = 0; i < section_count; ++i) {
        const uint8_t *p = module->file.data() + section_table_offset + i * 40;
        Section &s = module->sections[i];
        s.name = short_name(p, 8);
        s.virtual_size = rd32(p + 8);
        s.virtual_address = rd32(p + 12);
        s.raw_size = rd32(p + 16);
        s.raw_ptr = rd32(p + 20);
        s.characteristics = rd32(p + 36);
        /* Match Windows' PE section protection derivation:
         *   R -> read, W -> read+write, X -> execute-only,
         *   R|X -> read+execute, no R/W/X -> PAGE_NOACCESS.
         * W+X is rejected by this bounded loader even though Windows maps
         * PAGE_EXECUTE_READWRITE.  Never invent READ from !MEM_WRITE. */
        bool raw_read = (s.characteristics & kScnMemRead) != 0;
        bool raw_write = (s.characteristics & kScnMemWrite) != 0;
        bool raw_exec = (s.characteristics & kScnMemExecute) != 0;
        if (raw_write && raw_exec) {
            if (error) *error = "W+X section is not supported: " + s.name;
            return false;
        }
        s.readable = raw_read || raw_write;
        s.writable = raw_write;
        s.executable = raw_exec;
        if (s.virtual_address % module->section_alignment != 0) {
            if (error) *error = "section VA is not SectionAlignment-aligned: " + s.name;
            return false;
        }
        if (s.virtual_address < module->size_of_headers) {
            if (error) *error = "section overlaps headers: " + s.name;
            return false;
        }
        s.effective_virtual_size = s.virtual_size ? s.virtual_size : s.raw_size;
        if (s.effective_virtual_size == 0) {
            if (error) *error = "section has zero virtual size: " + s.name;
            return false;
        }
        uint64_t mapped = align_up(s.effective_virtual_size, module->section_alignment);
        if (mapped > UINT32_MAX ||
            (uint64_t)s.virtual_address + mapped > module->size_of_image) {
            if (error) *error = "section virtual range exceeds SizeOfImage: " + s.name;
            return false;
        }
        s.mapped_size = (uint32_t)mapped;
        if (s.raw_size > 0) {
            if (!fits(s.raw_ptr, s.raw_size, module->file_size)) {
                if (error) *error = "section raw data outside file: " + s.name;
                return false;
            }
        }
    }
    std::vector<const Section *> sorted;
    for (const auto &s : module->sections)
        sorted.push_back(&s);
    std::sort(sorted.begin(), sorted.end(),
              [](const Section *a, const Section *b) {
                  return a->virtual_address < b->virtual_address;
              });
    for (size_t i = 1; i < sorted.size(); ++i) {
        uint64_t prev_end =
            (uint64_t)sorted[i - 1]->virtual_address + sorted[i - 1]->mapped_size;
        if (sorted[i]->virtual_address < prev_end) {
            if (error) *error = "overlapping sections: " + sorted[i - 1]->name +
                                   " / " + sorted[i]->name;
            return false;
        }
    }
    uint64_t headers_end = align_up(module->size_of_headers, 0x1000);
    if (!sorted.empty() && headers_end > sorted.front()->virtual_address) {
        if (error) *error = "aligned headers overlap first section";
        return false;
    }
    return true;
}

const PdataEntry *pdata_for_begin(const Module &module, uint32_t rva) {
    for (const auto &entry : module.pdata) {
        if (entry.begin == rva)
            return &entry;
    }
    return nullptr;
}

const PdataEntry *pdata_containing(const Module &module, uint32_t rva) {
    for (const auto &entry : module.pdata) {
        if (rva >= entry.begin && rva < entry.end)
            return &entry;
    }
    return nullptr;
}

bool parse_pdata(Module *module, uint32_t dir_rva, uint32_t dir_size,
                 std::string *error) {
    if (dir_size == 0)
        return true;
    if (dir_size % 12 != 0) {
        if (error) *error = "exception directory size is not a multiple of 12";
        return false;
    }
    uint32_t count = dir_size / 12;
    if (count > kMaxPdata) {
        if (error) *error = "too many .pdata entries";
        return false;
    }
    uint32_t offset = 0;
    if (!rva_to_file(*module, dir_rva, dir_size, &offset, error))
        return false;
    module->pdata.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *p = module->file.data() + offset + i * 12;
        PdataEntry e;
        e.begin = rd32(p);
        e.end = rd32(p + 4);
        if (e.begin >= e.end || e.end > module->size_of_image) {
            if (error) *error = "invalid .pdata range at index " + std::to_string(i);
            return false;
        }
        const Section *begin_sec = module->section_for_rva(e.begin);
        const Section *end_sec = module->section_for_rva(e.end - 1);
        if (!begin_sec || !end_sec || begin_sec != end_sec ||
            !begin_sec->executable) {
            if (error) *error = ".pdata range is not inside one executable section";
            return false;
        }
        module->pdata[i] = e;
    }
    std::sort(module->pdata.begin(), module->pdata.end(),
              [](const PdataEntry &a, const PdataEntry &b) {
                  return a.begin < b.begin;
              });
    for (size_t i = 1; i < module->pdata.size(); ++i) {
        if (module->pdata[i].begin < module->pdata[i - 1].end) {
            if (error) *error = "overlapping .pdata entries";
            return false;
        }
    }
    return true;
}

bool parse_exports(Module *module, uint32_t dir_rva, uint32_t dir_size,
                   std::string *error) {
    if (dir_size == 0) {
        if (error) *error = "PE has no export directory";
        return false;
    }
    uint32_t off = 0;
    if (!rva_to_file(*module, dir_rva, 40, &off, error))
        return false;
    if ((uint64_t)dir_rva + dir_size > module->size_of_image) {
        if (error) *error = "export directory exceeds image";
        return false;
    }
    const uint8_t *p = module->file.data() + off;
    uint32_t base = rd32(p + 16);
    uint32_t num_funcs = rd32(p + 20);
    uint32_t num_names = rd32(p + 24);
    uint32_t funcs_rva = rd32(p + 28);
    uint32_t names_rva = rd32(p + 32);
    uint32_t ords_rva = rd32(p + 36);
    if (num_funcs == 0 || num_names == 0 || num_funcs > kMaxExports ||
        num_names > kMaxExports || num_names > num_funcs) {
        if (error) *error = "export count is zero or exceeds limit";
        return false;
    }
    if ((uint64_t)dir_rva + dir_size > module->size_of_image) {
        if (error) *error = "export directory exceeds SizeOfImage";
        return false;
    }
    uint32_t funcs_off = 0, names_off = 0, ords_off = 0;
    if (!rva_to_file(*module, funcs_rva, num_funcs * 4, &funcs_off, error) ||
        !rva_to_file(*module, names_rva, num_names * 4, &names_off, error) ||
        !rva_to_file(*module, ords_rva, num_names * 2, &ords_off, error))
        return false;

    std::vector<uint8_t> named(num_funcs, 0);
    std::set<std::string> seen;
    std::vector<Export> exports;
    for (uint32_t i = 0; i < num_names; ++i) {
        uint32_t name_rva = rd32(module->file.data() + names_off + i * 4);
        uint16_t index = rd16(module->file.data() + ords_off + i * 2);
        if (index >= num_funcs) {
            if (error) *error = "export name ordinal out of range";
            return false;
        }
        std::string name;
        if (!read_cstring(*module, name_rva, &name, error))
            return false;
        if (name.empty() || name.size() > kMaxName) {
            if (error) *error = "invalid export name";
            return false;
        }
        if (!seen.insert(name).second) {
            if (error) *error = "duplicate export name: " + name;
            return false;
        }
        uint32_t func_rva = rd32(module->file.data() + funcs_off + index * 4);
        if (func_rva == 0)
            continue;
        if (func_rva >= dir_rva && func_rva < dir_rva + dir_size) {
            if (error) *error = "forwarder export is not supported: " + name;
            return false;
        }
        if (func_rva >= module->size_of_image) {
            if (error) *error = "export RVA outside image: " + name;
            return false;
        }
        const Section *sec = module->section_for_rva(func_rva);
        if (!sec || !sec->executable) {
            if (error) *error = "export is not in an executable section: " + name;
            return false;
        }
        if ((uint64_t)base + index > UINT32_MAX) {
            if (error) *error = "export ordinal overflows";
            return false;
        }
        named[index] = 1;
        exports.push_back(Export{name, func_rva, base + index});
    }
    std::sort(exports.begin(), exports.end(),
              [](const Export &a, const Export &b) { return a.name < b.name; });
    module->exports = std::move(exports);
    if (module->exports.empty()) {
        if (error) *error = "PE has no named exports";
        return false;
    }
    for (uint32_t index = 0; index < num_funcs; ++index) {
        uint32_t func_rva = rd32(module->file.data() + funcs_off + index * 4);
        if (func_rva == 0 || named[index])
            continue;
        if (func_rva >= module->size_of_image) {
            if (error) *error = "ordinal export RVA outside image";
            return false;
        }
        if (func_rva >= dir_rva && func_rva < (uint64_t)dir_rva + dir_size) {
            if (error) *error = "forwarder ordinal export is not supported";
            return false;
        }
        if ((uint64_t)base + index > UINT32_MAX) {
            if (error) *error = "ordinal export ordinal overflows";
            return false;
        }
        module->ordinal_only_exports.push_back(OrdinalExport{func_rva, base + index});
    }
    return true;
}

bool parse_imports(Module *module, uint32_t dir_rva, uint32_t dir_size,
                   std::string *error) {
    if (dir_size == 0) {
        if (dir_rva != 0) {
            if (error) *error = "import directory RVA is non-zero with zero size";
            return false;
        }
        return true;
    }
    if (dir_size < 20) {
        if (error) *error = "import directory is too small";
        return false;
    }
    uint32_t off = 0;
    if (!rva_to_file(*module, dir_rva, 20, &off, error))
        return false;
    std::map<std::string, uint32_t> seen;
    uint32_t iat_slots = 0;
    std::vector<Import> slots;
    bool terminated = false;
    for (uint32_t d = 0; d < kMaxImportDescriptors; ++d) {
        uint64_t desc_rva64 = (uint64_t)dir_rva + (uint64_t)d * 20;
        if (d * 20 + 20 > dir_size || desc_rva64 > UINT32_MAX) {
            if (error) *error = "import descriptor outside directory";
            return false;
        }
        uint32_t desc_off = 0;
        if (!rva_to_file(*module, (uint32_t)desc_rva64, 20, &desc_off, error))
            return false;
        const uint8_t *desc = module->file.data() + desc_off;
        uint32_t original_first = rd32(desc);
        uint32_t name_rva = rd32(desc + 12);
        uint32_t first_thunk = rd32(desc + 16);
        if (original_first == 0 && name_rva == 0 && first_thunk == 0) {
            terminated = true;
            break;
        }
        if (name_rva == 0 || first_thunk == 0) {
            if (error) *error = "invalid import descriptor";
            return false;
        }
        std::string module_name;
        if (!read_cstring(*module, name_rva, &module_name, error))
            return false;
        if (module_name.empty() || module_name.size() > 256) {
            if (error) *error = "invalid import module name";
            return false;
        }
        if (module_name.rfind("api-ms-", 0) == 0 ||
            module_name.rfind("ext-ms-", 0) == 0) {
            if (error) *error = "API set import is not supported: " + module_name;
            return false;
        }
        uint32_t lookup = original_first ? original_first : first_thunk;
        for (uint32_t t = 0; t < kMaxIatSlots; ++t) {
            uint64_t thunk_rva64 = (uint64_t)lookup + (uint64_t)t * 8;
            if (thunk_rva64 > UINT32_MAX) {
                if (error) *error = "import lookup table overflows";
                return false;
            }
            uint32_t thunk_off = 0;
            if (!rva_to_file(*module, (uint32_t)thunk_rva64, 8, &thunk_off, error))
                return false;
            uint64_t value = rd64(module->file.data() + thunk_off);
            if (value == 0)
                break;
            if (value & 0x8000000000000000ULL) {
                if (error) *error = "ordinal import is not supported: " + module_name;
                return false;
            }
            if ((uint64_t)(uint32_t)value + 2 > module->size_of_image) {
                if (error) *error = "import name RVA outside image";
                return false;
            }
            uint32_t hint_rva = (uint32_t)value;
            std::string symbol;
            if (!read_cstring(*module, hint_rva + 2, &symbol, error))
                return false;
            if (symbol.empty() || symbol.size() > kMaxName) {
                if (error) *error = "invalid import symbol name";
                return false;
            }
            uint64_t iat_rva64 = (uint64_t)first_thunk + (uint64_t)t * 8;
            if (iat_rva64 > UINT32_MAX || iat_rva64 + 8 > module->size_of_image) {
                if (error) *error = "IAT slot outside image";
                return false;
            }
            uint32_t iat_rva = (uint32_t)iat_rva64;
            std::string key = module_name + "\n" + symbol;
            auto it = seen.find(key);
            if (it == seen.end()) {
                if (seen.size() >= kMaxUniqueImports) {
                    if (error) *error = "too many unique imports";
                    return false;
                }
                uint32_t index = (uint32_t)seen.size();
                uint64_t thunk_va = module->thunk_base + index * kThunkStride;
                seen.emplace(key, index);
                Import import;
                import.module = module_name;
                import.symbol = symbol;
                import.iat_rva = iat_rva;
                import.thunk_va = thunk_va;
                import.thunk_index = index;
                module->unique_imports.push_back(import);
                slots.push_back(import);
            } else {
                Import import = module->unique_imports[it->second];
                import.iat_rva = iat_rva;
                slots.push_back(import);
            }
            ++iat_slots;
            if (iat_slots > kMaxIatSlots) {
                if (error) *error = "too many IAT slots";
                return false;
            }
        }
    }
    if (!terminated) {
        if (error) *error = "import descriptor table is not terminated";
        return false;
    }
    module->imports = std::move(slots);
    return true;
}

bool parse_relocations(Module *module, uint32_t dir_rva, uint32_t dir_size,
                       std::string *error) {
    if (dir_size == 0) {
        module->relocs_stripped = true;
        return true;
    }
    if (dir_size < 8) {
        if (error) *error = "relocation directory is too small";
        return false;
    }
    uint32_t off = 0;
    if (!rva_to_file(*module, dir_rva, dir_size, &off, error))
        return false;
    const uint64_t dir_end = dir_size;
    uint64_t pos = 0;
    uint64_t total_entries = 0;
    while (dir_end - pos >= 8) {
        const uint8_t *block = module->file.data() + off + pos;
        uint32_t page = rd32(block);
        uint64_t size = rd32(block + 4);
        if (size < 8 || (size - 8) % 2 != 0 || size > dir_end - pos) {
            if (error) *error = "invalid relocation block";
            return false;
        }
        uint64_t entries = (size - 8) / 2;
        total_entries += entries;
        if (total_entries > 2ULL * kMaxRelocations) {
            if (error) *error = "too many relocation entries";
            return false;
        }
        for (uint64_t i = 0; i < entries; ++i) {
            uint64_t entry_offset = (uint64_t)off + pos + 8 + i * 2;
            if (entry_offset + 2 > (uint64_t)off + dir_end) {
                if (error) *error = "relocation entry outside directory";
                return false;
            }
            uint16_t entry = rd16(module->file.data() + entry_offset);
            uint16_t type = entry >> 12;
            uint16_t offset = entry & 0x0fff;
            if (type == 0)
                continue;
            if (type != 10) {
                if (error) *error = "unsupported relocation type " +
                                     std::to_string(type) + " at " + hex32(page + offset);
                return false;
            }
            uint64_t site = (uint64_t)page + offset;
            if (site > UINT32_MAX || site + 8 > module->size_of_image) {
                if (error) *error = "relocation site outside image";
                return false;
            }
            if (!module->section_for_rva((uint32_t)site)) {
                if (error) *error = "relocation site is not in a mapped section";
                return false;
            }
            if (module->relocations.size() >= kMaxRelocations) {
                if (error) *error = "too many relocation sites";
                return false;
            }
            module->relocations.push_back(Relocation{(uint32_t)site, type});
        }
        pos += size;
    }
    if (pos != dir_end) {
        if (error) *error = "relocation directory has trailing bytes";
        return false;
    }
    return true;
}

void compute_identity(Module *module, const ImportRegistry &registry) {
    aev::sha256(module->image.data(), module->image.size(), module->image_sha256);

    std::vector<uint8_t> import_buf;
    const char import_tag[] = "AEVPEIMPORT1\n";
    import_buf.insert(import_buf.end(), import_tag, import_tag + sizeof(import_tag) - 1);
    append_u32(&import_buf, (uint32_t)module->unique_imports.size());
    for (const auto &imp : module->unique_imports) {
        append_bytes(&import_buf, imp.module);
        append_bytes(&import_buf, imp.symbol);
        append_u64(&import_buf, imp.thunk_va);
    }
    for (size_t i = 0; i < module->unique_imports.size(); ++i) {
        append_u64(&import_buf, module->thunk_base + i * kThunkStride);
        append_u64(&import_buf, 0);
    }
    aev::sha256(import_buf.data(), import_buf.size(), module->import_binding_sha256);

    std::memcpy(module->host_binding_sha256, registry.binding_sha256(), 32);

    std::vector<std::string> names;
    names.reserve(module->exports.size());
    for (const auto &e : module->exports)
        names.push_back(e.name);
    std::sort(names.begin(), names.end());
    std::vector<uint8_t> entry_buf;
    const char entry_tag[] = "AEVPEENTRY1\n";
    entry_buf.insert(entry_buf.end(), entry_tag, entry_tag + sizeof(entry_tag) - 1);
    append_u32(&entry_buf, (uint32_t)names.size());
    for (const auto &name : names)
        append_bytes(&entry_buf, name);
    aev::sha256(entry_buf.data(), entry_buf.size(), module->entry_exports_sha256);
}

bool build_regions(Module *module, uint64_t stack_size, std::string *error) {
    module->regions.clear();
    uint64_t image_end = module->load_base + module->size_of_image;
    if (image_end < module->load_base) {
        if (error) *error = "load_base + SizeOfImage overflows";
        return false;
    }
    uint64_t headers_size = align_up(module->size_of_headers, 0x1000);
    if (headers_size == 0 || headers_size > module->size_of_image) {
        if (error) *error = "headers region exceeds image";
        return false;
    }
    module->regions.push_back(AevMemRegion{module->load_base, headers_size,
                                           (uint64_t)(uintptr_t)module->image.data(),
                                           AEV_MEM_PROT_READ, 0});
    for (const auto &s : module->sections) {
        uint32_t prot = 0;
        if (s.readable) prot |= AEV_MEM_PROT_READ;
        if (s.writable) prot |= AEV_MEM_PROT_WRITE;
        if (s.executable) prot |= AEV_MEM_PROT_EXEC;
        /* prot 0 is a valid PAGE_NOACCESS region for R/W/X-empty sections;
         * every access is rejected by the region lookup, not by silently
         * granting READ. */
        if ((uint64_t)s.virtual_address + s.mapped_size > module->size_of_image) {
            if (error) *error = "section region exceeds image: " + s.name;
            return false;
        }
        module->regions.push_back(AevMemRegion{
            module->load_base + s.virtual_address, s.mapped_size,
            (uint64_t)(uintptr_t)(module->image.data() + s.virtual_address), prot, 0});
    }

    uint64_t thunk_size = 0;
    if (!module->unique_imports.empty()) {
        thunk_size = align_up(module->unique_imports.size() * kThunkStride, 0x1000);
        if (thunk_size == 0 || module->thunk_base > UINT64_MAX - thunk_size) {
            if (error) *error = "thunk region overflows";
            return false;
        }
        module->thunk_host.assign((size_t)thunk_size, 0);
        for (size_t i = 0; i < module->unique_imports.size(); ++i) {
            uint64_t va = module->thunk_base + i * kThunkStride;
            std::memcpy(module->thunk_host.data() + i * kThunkStride, &va, 8);
            uint64_t zero = 0;
            std::memcpy(module->thunk_host.data() + i * kThunkStride + 8, &zero, 8);
        }
        module->regions.push_back(AevMemRegion{
            module->thunk_base, thunk_size,
            (uint64_t)(uintptr_t)module->thunk_host.data(),
            AEV_MEM_PROT_READ | AEV_MEM_PROT_EXEC, 0});
    } else {
        module->thunk_base = 0;
        module->thunk_host.clear();
    }

    uint64_t stack_base = std::max(module->load_base + module->size_of_image,
                                   module->thunk_base + thunk_size);
    stack_base = align_up(stack_base, 0x1000);
    uint64_t aligned_stack = align_up(stack_size ? stack_size : kStackSize, 0x1000);
    if (aligned_stack < 0x10000)
        aligned_stack = 0x10000;
    if (stack_base > UINT64_MAX - aligned_stack) {
        if (error) *error = "stack region overflows";
        return false;
    }
    module->stack_host.assign((size_t)aligned_stack, 0);
    module->stack_base = stack_base;
    module->stack_top = stack_base + aligned_stack;
    if (module->stack_top < 0x108) {
        if (error) *error = "stack too small for entry frame";
        return false;
    }
    module->entry_rsp = module->stack_top - 0x108;
    module->regions.push_back(AevMemRegion{
        module->stack_base, aligned_stack,
        (uint64_t)(uintptr_t)module->stack_host.data(),
        AEV_MEM_PROT_READ | AEV_MEM_PROT_WRITE, 0});

    std::sort(module->regions.begin(), module->regions.end(),
              [](const AevMemRegion &a, const AevMemRegion &b) {
                  return a.guest_base < b.guest_base;
              });
    for (size_t i = 1; i < module->regions.size(); ++i) {
        if (module->regions[i].guest_base <
            module->regions[i - 1].guest_base + module->regions[i - 1].size) {
            if (error) *error = "Guest memory regions overlap";
            return false;
        }
    }
    return true;
}

}  // namespace

const Section *Module::section_for_rva(uint32_t rva) const {
    for (const auto &s : sections) {
        if (rva >= s.virtual_address &&
            (uint64_t)rva < (uint64_t)s.virtual_address + s.mapped_size)
            return &s;
    }
    return nullptr;
}

const Export *Module::find_export(const std::string &name) const {
    for (const auto &e : exports) {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

bool ImportRegistry::load(const std::string &path, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open import registry: " + path;
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    entries_.clear();
    std::set<std::pair<std::string, std::string>> seen;
    std::set<unsigned long long> seen_ids;
    size_t line_no = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> fields;
        std::string current;
        for (char c : line) {
            if (c == '|') {
                fields.push_back(trim(current));
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        fields.push_back(trim(current));
        if (fields.size() != 5) {
            if (error) *error = "registry line " + std::to_string(line_no) +
                                 ": expected 5 fields";
            return false;
        }
        const std::string &module = fields[0];
        const std::string &symbol = fields[1];
        if (module.empty() || symbol.empty() || module.size() > 256 ||
            symbol.size() > 512) {
            if (error) *error = "registry line " + std::to_string(line_no) +
                                 ": invalid module/symbol";
            return false;
        }
        char *end = nullptr;
        unsigned long long id = std::strtoull(fields[2].c_str(), &end, 0);
        if (!end || *end != 0 || id == 0 || id > 0xffffffffULL) {
            if (error) *error = "registry line " + std::to_string(line_no) +
                                 ": invalid host_import_id";
            return false;
        }
        unsigned long args = std::strtoul(fields[3].c_str(), &end, 0);
        if (!end || *end != 0 || args > 4) {
            if (error) *error = "registry line " + std::to_string(line_no) +
                                 ": invalid args";
            return false;
        }
        if (fields[4] != "win64-u64") {
            if (error) *error = "registry line " + std::to_string(line_no) +
                                 ": unsupported ABI " + fields[4];
            return false;
        }
        if (!seen.insert({module, symbol}).second) {
            if (error) *error = "registry line " + std::to_string(line_no) +
                                 ": duplicate import";
            return false;
        }
        if (!seen_ids.insert(id).second) {
            if (error) *error = "registry line " + std::to_string(line_no) +
                                 ": duplicate host_import_id";
            return false;
        }
        entries_.push_back({{module, symbol},
                            HostImportDecl{(uint64_t)id, (uint32_t)args, fields[4]}});
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    if (entries_.size() > kMaxUniqueImports) {
        if (error) *error = "too many registry entries";
        return false;
    }
    std::vector<uint8_t> buf;
    const char tag[] = "AEVPEHOST1\n";
    buf.insert(buf.end(), tag, tag + sizeof(tag) - 1);
    append_u32(&buf, (uint32_t)entries_.size());
    for (const auto &entry : entries_) {
        append_bytes(&buf, entry.first.first);
        append_bytes(&buf, entry.first.second);
        append_u64(&buf, entry.second.id);
        append_u32(&buf, entry.second.args);
        append_bytes(&buf, entry.second.abi);
    }
    aev::sha256(buf.data(), buf.size(), binding_sha256_);
    uint8_t file_sha[32];
    std::string hash_error;
    if (!aev::sha256_file(path, file_sha, &hash_error)) {
        if (error) *error = "cannot hash registry: " + hash_error;
        return false;
    }
    file_sha256_ = aev::hex_bytes(file_sha, 32);
    return true;
}

bool ImportRegistry::resolve(const std::string &module, const std::string &symbol,
                             HostImportDecl *out) const {
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), std::make_pair(module, symbol),
        [](const auto &entry, const auto &key) { return entry.first < key; });
    if (it == entries_.end() || it->first.first != module ||
        it->first.second != symbol)
        return false;
    if (out) *out = it->second;
    return true;
}

bool load_module(const std::string &path, const LoadOptions &options,
                 const ImportRegistry &registry, Module *module,
                 std::string *error) {
    if (!module) {
        if (error) *error = "null module";
        return false;
    }
    Module result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open PE file: " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 64 || (uint64_t)size > kMaxFileSize) {
        if (error) *error = "PE file size is invalid";
        return false;
    }
    result.file.resize((size_t)size);
    in.read(reinterpret_cast<char *>(result.file.data()), size);
    if (!in) {
        if (error) *error = "cannot read PE file";
        return false;
    }
    result.file_size = (uint64_t)result.file.size();
    result.file_fnv = fnv1a(result.file);
    aev::sha256(result.file.data(), result.file.size(), result.file_sha256);

    const uint8_t *base = result.file.data();
    if (base[0] != 'M' || base[1] != 'Z') {
        if (error) *error = "not a PE file (missing MZ)";
        return false;
    }
    uint32_t pe_off = rd32(base + 0x3c);
    if (!fits(pe_off, 24, result.file_size) ||
        std::memcmp(base + pe_off, "PE\0\0", 4) != 0) {
        if (error) *error = "invalid PE signature";
        return false;
    }
    const uint8_t *coff = base + pe_off + 4;
    result.machine = rd16(coff);
    uint16_t section_count = rd16(coff + 2);
    uint16_t optional_size = rd16(coff + 16);
    result.characteristics = rd16(coff + 18);
    if (result.machine != kMachineAmd64) {
        if (error) *error = "not an AMD64 PE (machine=" +
                             std::to_string(result.machine) + ")";
        return false;
    }
    if (!(result.characteristics & kFileDll)) {
        if (error) *error = "input is not a DLL";
        return false;
    }
    if (result.characteristics & (kFile32BitMachine | kFileRelocsStripped)) {
        /* RELOCS_STRIPPED is handled below; the 32-bit flag is fatal. */
        if (result.characteristics & kFile32BitMachine) {
            if (error) *error = "32-bit PE is not supported";
            return false;
        }
    }
    uint64_t opt_off = (uint64_t)pe_off + 24;
    if (!fits(opt_off, optional_size, result.file_size) || optional_size < 112) {
        if (error) *error = "optional header outside file/too small";
        return false;
    }
    const uint8_t *opt = base + opt_off;
    result.magic = rd16(opt);
    if (result.magic != kMagicPe32Plus) {
        if (error) *error = "not a PE32+ optional header";
        return false;
    }
    result.entry_rva = rd32(opt + 16);
    result.preferred_base = rd64(opt + 24);
    result.section_alignment = rd32(opt + 32);
    result.file_alignment = rd32(opt + 36);
    result.size_of_image = rd32(opt + 56);
    result.size_of_headers = rd32(opt + 60);
    uint16_t dll_characteristics = rd16(opt + 70);
    uint32_t num_dirs = rd32(opt + 108);
    if (result.section_alignment < 0x1000 ||
        result.section_alignment % 0x1000 != 0) {
        if (error) *error = "SectionAlignment is not a 4 KiB multiple";
        return false;
    }
    if (result.file_alignment < 0x200 || result.file_alignment > 0x10000 ||
        (result.file_alignment & (result.file_alignment - 1)) != 0) {
        if (error) *error = "invalid FileAlignment";
        return false;
    }
    if (result.size_of_headers == 0 || result.size_of_headers > (1u << 20) ||
        result.size_of_headers % result.file_alignment != 0 ||
        result.size_of_headers > result.file_size) {
        if (error) *error = "invalid SizeOfHeaders";
        return false;
    }
    if (result.size_of_image == 0 || result.size_of_image > kMaxImageSize ||
        result.size_of_image % result.section_alignment != 0 ||
        result.size_of_headers > result.size_of_image) {
        if (error) *error = "invalid SizeOfImage";
        return false;
    }
    if (result.entry_rva != 0) {
        if (error) *error = "DllMain/entry point is not supported";
        return false;
    }
    if (dll_characteristics & kDllGuardCf) {
        if (error) *error = "CFG (GUARD_CF) is not supported";
        return false;
    }
    if (num_dirs > 16) {
        if (error) *error = "too many data directories";
        return false;
    }
    if (112 + (uint64_t)num_dirs * 8 > optional_size ||
        !fits(opt_off + 112, (uint64_t)num_dirs * 8, result.file_size)) {
        if (error) *error = "data directory array outside optional header";
        return false;
    }
    auto dir_rva = [&](uint32_t index) -> uint32_t {
        return index < num_dirs ? rd32(opt + 112 + index * 8) : 0;
    };
    auto dir_size = [&](uint32_t index) -> uint32_t {
        return index < num_dirs ? rd32(opt + 112 + index * 8 + 4) : 0;
    };
    if (dir_size(kDirTls) != 0) {
        if (error) *error = "TLS directory is not supported";
        return false;
    }
    if (dir_size(kDirDelayImport) != 0) {
        if (error) *error = "delay import directory is not supported";
        return false;
    }
    if (dir_size(kDirBoundImport) != 0) {
        if (error) *error = "bound import directory is not supported";
        return false;
    }
    if (dir_size(kDirLoadConfig) != 0) {
        if (error) *error = "load config/CFG is not supported";
        return false;
    }

    uint64_t section_table = opt_off + optional_size;
    if (!parse_sections(&result, (uint32_t)section_table, section_count, error))
        return false;

    result.load_base = options.load_base;
    if (result.load_base == 0 || result.load_base % 0x10000 != 0) {
        if (error) *error = "load base must be non-zero and 64 KiB aligned";
        return false;
    }
    if (result.load_base > UINT64_MAX - result.size_of_image) {
        if (error) *error = "load base + SizeOfImage overflows";
        return false;
    }


    if (dir_size(kDirException) != 0) {
        if (!parse_pdata(&result, dir_rva(kDirException), dir_size(kDirException),
                         error))
            return false;
    }
    if (!parse_exports(&result, dir_rva(kDirExport), dir_size(kDirExport), error))
        return false;

    result.thunk_base = align_up(result.load_base + result.size_of_image, 0x1000);
    if (result.thunk_base < result.load_base) {
        if (error) *error = "thunk base overflows";
        return false;
    }
    if (!parse_imports(&result, dir_rva(kDirImport), dir_size(kDirImport), error))
        return false;
    for (auto &imp : result.unique_imports) {
        HostImportDecl decl;
        if (!registry.resolve(imp.module, imp.symbol, &decl)) {
            if (error) *error = "unknown import: " + imp.module + "!" + imp.symbol;
            return false;
        }
        imp.host_import_id = decl.id;
        imp.args = decl.args;
        imp.abi = decl.abi;
    }
    for (auto &imp : result.imports) {
        HostImportDecl decl;
        if (!registry.resolve(imp.module, imp.symbol, &decl)) {
            if (error) *error = "unknown import: " + imp.module + "!" + imp.symbol;
            return false;
        }
        imp.host_import_id = decl.id;
        imp.args = decl.args;
        imp.abi = decl.abi;
    }
    if (result.unique_imports.empty())
        result.thunk_base = 0;
    result.import_thunks.reserve(result.unique_imports.size());
    for (const auto &imp : result.unique_imports)
        result.import_thunks.push_back(imp.thunk_va);

    if (!parse_relocations(&result, dir_rva(kDirBaseReloc), dir_size(kDirBaseReloc),
                           error))
        return false;
    if ((result.characteristics & kFileRelocsStripped) != 0)
        result.relocs_stripped = true;
    if (result.relocs_stripped && result.load_base != result.preferred_base) {
        if (error) *error = "relocations are stripped and load base != preferred base";
        return false;
    }

    result.image.assign(result.size_of_image, 0);
    std::memcpy(result.image.data(), result.file.data(),
                std::min<uint64_t>(result.size_of_headers, result.file_size));
    for (const auto &s : result.sections) {
        uint64_t copy = std::min<uint64_t>(s.raw_size, s.effective_virtual_size);
        if (copy == 0)
            continue;
        std::memcpy(result.image.data() + s.virtual_address,
                    result.file.data() + s.raw_ptr, (size_t)copy);
    }
    uint64_t delta = result.load_base - result.preferred_base;
    for (const auto &reloc : result.relocations) {
        uint64_t value = 0;
        std::memcpy(&value, result.image.data() + reloc.rva, 8);
        value += delta;
        std::memcpy(result.image.data() + reloc.rva, &value, 8);
    }
    for (const auto &imp : result.imports) {
        uint64_t value = imp.thunk_va;
        std::memcpy(result.image.data() + imp.iat_rva, &value, 8);
    }

    compute_identity(&result, registry);
    if (!build_regions(&result, options.stack_size, error))
        return false;

    *module = std::move(result);
    return true;
}

bool discover_functions(const Module &module,
                        const std::vector<std::string> &root_exports,
                        std::vector<FunctionSpec> *specs,
                        std::string *error) {
    if (!specs) {
        if (error) *error = "null specs";
        return false;
    }
    specs->clear();
    if (root_exports.empty()) {
        if (error) *error = "no root exports requested";
        return false;
    }
    std::map<uint32_t, std::string> export_names;
    for (const auto &e : module.exports)
        export_names[e.rva] = e.name;

    std::vector<uint32_t> queue;
    std::set<uint32_t> queued;
    for (const auto &name : root_exports) {
        const Export *e = module.find_export(name);
        if (!e) {
            if (error) *error = "export not found: " + name;
            return false;
        }
        if (!pdata_for_begin(module, e->rva)) {
            if (error) *error = "export is not a .pdata function start: " + name;
            return false;
        }
        if (queued.insert(e->rva).second)
            queue.push_back(e->rva);
    }

    std::set<uint32_t> visited;
    std::vector<FunctionSpec> out;
    uint64_t total_code = 0;
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        uint32_t rva = queue[qi];
        if (!visited.insert(rva).second)
            continue;
        if (out.size() >= kMaxFunctions) {
            if (error) *error = "too many discovered functions";
            return false;
        }
        const PdataEntry *entry = pdata_for_begin(module, rva);
        if (!entry) {
            if (error) *error = "direct target is not a .pdata function start: " +
                                 hex32(rva);
            return false;
        }
        uint32_t size = entry->end - entry->begin;
        if (size == 0 || size > kMaxFunctionCode ||
            (uint64_t)rva + size > module.size_of_image) {
            if (error) *error = "function size is invalid at " + hex32(rva);
            return false;
        }
        total_code += size;
        if (total_code > kMaxTotalCode) {
            if (error) *error = "total discovered code exceeds limit";
            return false;
        }
        std::vector<uint8_t> bytes(module.image.begin() + rva,
                                   module.image.begin() + rva + size);
        uint64_t fn_va = module.load_base + rva;
        DecodeResult dr = decode_x64(bytes, fn_va);
        if (!dr.ok) {
            if (error) *error = "decode failed at " + hex32(rva) + ": " + dr.error;
            return false;
        }
        if (dr.insns.empty()) {
            if (error) *error = "function has no instructions at " + hex32(rva);
            return false;
        }
        size_t keep = dr.insns.size();
        while (keep >= 2) {
            const X64Insn &last = dr.insns[keep - 1];
            const X64Insn &prev = dr.insns[keep - 2];
            bool padding = last.id == X86_INS_INT3 || last.id == X86_INS_NOP;
            bool terminator = prev.id == X86_INS_RET || prev.id == X86_INS_JMP;
            if (!(padding && terminator))
                break;
            --keep;
        }
        if (keep == 0) {
            if (error) *error = "function is only padding at " + hex32(rva);
            return false;
        }
        uint64_t code_end = dr.insns[keep - 1].address + dr.insns[keep - 1].size;
        if (code_end <= fn_va || code_end - fn_va > UINT32_MAX) {
            if (error) *error = "invalid trimmed function size";
            return false;
        }
        uint32_t code_size = (uint32_t)(code_end - fn_va);
        std::string name;
        auto it = export_names.find(rva);
        if (it != export_names.end())
            name = it->second;
        else
            name = "sub_" + hex32(rva).substr(2);
        out.push_back(FunctionSpec{name, 1, fn_va, rva, code_size,
                                    entry->begin, entry->end});

        for (size_t i = 0; i < keep; ++i) {
            const X64Insn &insn = dr.insns[i];
            if ((insn.id != X86_INS_CALL && insn.id != X86_INS_JMP) ||
                insn.operands.empty() ||
                insn.operands[0].kind != OperandKind::Immediate)
                continue;
            uint64_t target = (uint64_t)insn.operands[0].imm;
            if (target >= fn_va && target < fn_va + code_size)
                continue;
            if (target < module.load_base) {
                if (error) *error = "direct target below module base at " + hex32(rva);
                return false;
            }
            uint64_t target_rva64 = target - module.load_base;
            if (target_rva64 > UINT32_MAX) {
                if (error) *error = "direct target outside module";
                return false;
            }
            uint32_t target_rva = (uint32_t)target_rva64;
            const PdataEntry *target_entry = pdata_for_begin(module, target_rva);
            if (!target_entry) {
                if (error) *error = "direct target is not a .pdata function start: " +
                                     hex32(target_rva);
                return false;
            }
            if (queued.insert(target_rva).second)
                queue.push_back(target_rva);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const FunctionSpec &a, const FunctionSpec &b) {
                  return a.rva < b.rva;
              });
    *specs = std::move(out);
    return true;
}

bool build_program_for_module(Module &module,
                              const std::vector<FunctionSpec> &specs,
                              Program *program, std::string *error) {
    CoffObject object;
    object.machine = 0x8664;
    object.content_hash = module.file_fnv;
    object.content_size = module.file_size;
    std::memcpy(object.content_sha256, module.file_sha256, 32);
    CoffSection image_section;
    image_section.name = ".pe_image";
    image_section.virtual_address = 0;
    image_section.virtual_size = module.size_of_image;
    image_section.characteristics = kScnMemRead | kScnMemExecute;
    image_section.data = module.image;
    object.sections.push_back(std::move(image_section));
    std::set<uint64_t> external;
    for (const auto &spec : specs)
        external.insert(spec.guest_va);
    return build_program_from_specs(std::move(object), specs, &external, program,
                                    error);
}

std::string hex_u64(uint64_t value) { return hex(value); }

}  // namespace aev::pe
