#include "coff.hpp"

#include "sha256.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace aev {
namespace {

uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

std::string short_name(const uint8_t *p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(reinterpret_cast<const char *>(p), len);
}

}  // namespace

bool CoffObject::load(const std::string &path, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 20) {
        if (error) *error = "file too small for COFF header";
        return false;
    }
    file_.resize((size_t)size);
    in.read(reinterpret_cast<char *>(file_.data()), size);
    if (!in) {
        if (error) *error = "cannot read " + path;
        return false;
    }

    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t byte : file_) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    content_hash = hash;
    content_size = (uint64_t)file_.size();
    sha256(file_.data(), file_.size(), content_sha256);

    const uint8_t *base = file_.data();
    machine = rd16(base + 0);
    uint16_t nsections = rd16(base + 2);
    timestamp = rd32(base + 4);
    uint32_t sym_ptr = rd32(base + 8);
    uint32_t nsyms = rd32(base + 12);
    uint16_t opt_size = rd16(base + 16);

    if (machine != 0x8664) {
        if (error) *error = "not an x86-64 COFF object (machine=" + std::to_string(machine) + ")";
        return false;
    }

    size_t section_off = 20 + opt_size;
    if (section_off + (size_t)nsections * 40 > file_.size()) {
        if (error) *error = "COFF section table is out of bounds";
        return false;
    }

    std::vector<std::string> raw_names;
    sections.resize(nsections);
    for (uint16_t i = 0; i < nsections; ++i) {
        const uint8_t *p = base + section_off + (size_t)i * 40;
        CoffSection &s = sections[i];
        s.name = short_name(p, 8);
        s.virtual_size = rd32(p + 8);
        s.virtual_address = rd32(p + 12);
        s.raw_size = rd32(p + 16);
        s.raw_ptr = rd32(p + 20);
        s.characteristics = rd32(p + 36);
        if (s.raw_size && s.raw_ptr + (uint64_t)s.raw_size <= file_.size()) {
            s.data.assign(base + s.raw_ptr, base + s.raw_ptr + s.raw_size);
        }
        raw_names.push_back(s.name);
    }

    std::string sym_string_table;
    if (sym_ptr && nsyms) {
        uint64_t sym_bytes = (uint64_t)nsyms * 18;
        if ((uint64_t)sym_ptr + sym_bytes > file_.size()) {
            if (error) *error = "COFF symbol table is out of bounds";
            return false;
        }
        if ((uint64_t)sym_ptr + sym_bytes + 4 <= file_.size()) {
            uint32_t str_size = rd32(base + sym_ptr + sym_bytes);
            uint64_t str_off = (uint64_t)sym_ptr + sym_bytes;
            if (str_size >= 4 && str_off + str_size <= file_.size()) {
                sym_string_table.assign(reinterpret_cast<const char *>(base + str_off),
                                        str_size);
            }
        }

        for (uint32_t i = 0; i < nsyms;) {
            const uint8_t *p = base + sym_ptr + (uint64_t)i * 18;
            CoffSymbol sym;
            if (rd32(p) == 0) {
                uint32_t off = rd32(p + 4);
                if (off < sym_string_table.size()) {
                    size_t len = 0;
                    while (off + len < sym_string_table.size() &&
                           sym_string_table[off + len])
                        ++len;
                    sym.name.assign(sym_string_table.data() + off, len);
                }
            } else {
                sym.name = short_name(p, 8);
            }
            sym.value = rd32(p + 8);
            sym.section_number = (int16_t)rd16(p + 12);
            sym.type = rd16(p + 14);
            sym.storage_class = p[16];
            sym.aux_count = p[17];
            symbols.push_back(std::move(sym));
            i += 1u + (uint32_t)p[17];
        }
    }

    /* Resolve long section names now that the string table is known. */
    for (size_t i = 0; i < sections.size(); ++i) {
        const std::string &n = raw_names[i];
        if (n.size() > 1 && n[0] == '/' && sym_string_table.size() > 4) {
            char *end = nullptr;
            unsigned long off = std::strtoul(n.c_str() + 1, &end, 10);
            if (end && *end == '\0' && off < sym_string_table.size()) {
                size_t len = 0;
                while (off + len < sym_string_table.size() &&
                       sym_string_table[off + len])
                    ++len;
                sections[i].name.assign(sym_string_table.data() + off, len);
            }
        }
    }

    return true;
}

const CoffSection *CoffObject::section(int index) const {
    if (index <= 0 || (size_t)index > sections.size())
        return nullptr;
    return &sections[(size_t)index - 1];
}

const CoffSymbol *CoffObject::find_symbol(const std::string &name) const {
    for (const auto &s : symbols) {
        if (s.name == name)
            return &s;
    }
    return nullptr;
}

}  // namespace aev
