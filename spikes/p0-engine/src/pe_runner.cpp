#include "aev_guest.h"
#include "jit_runtime.hpp"
#include "pe.hpp"
#include "program_identity.hpp"
#include "sha256.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pe = aev::pe;

namespace {

using AotRunFn = int (*)(GuestState *, const AevRuntime *);

enum PeRunStatus {
    PE_OK = 0,
    PE_INVALID_ARGUMENT = 2,
    PE_LOAD = 3,
    PE_ABI_MISMATCH = 4,
    PE_UNKNOWN_IMPORT = 5,
    PE_MEMORY = 6,
    PE_JIT = 7,
    PE_STEP_LIMIT = 8,
    PE_STALE_HANDLE = 9,
    PE_REENTRY = 10,
    PE_HOST = 11,
    PE_INTERNAL = 12
};

const char *status_name(int status) {
    switch (status) {
        case PE_OK: return "OK";
        case PE_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case PE_LOAD: return "LOAD";
        case PE_ABI_MISMATCH: return "ABI_MISMATCH";
        case PE_UNKNOWN_IMPORT: return "UNKNOWN_IMPORT";
        case PE_MEMORY: return "MEMORY";
        case PE_JIT: return "JIT";
        case PE_STEP_LIMIT: return "STEP_LIMIT";
        case PE_STALE_HANDLE: return "STALE_HANDLE";
        case PE_REENTRY: return "REENTRY";
        case PE_HOST: return "HOST";
        case PE_INTERNAL: return "INTERNAL";
        default: return "UNKNOWN";
    }
}

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string hex(uint64_t v) { return aev::hex64(v); }
std::string hex32(uint32_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

struct HostImpl {
    uint64_t id = 0;
    uint32_t args = 0;
    void *ctx = nullptr;
    std::function<int(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *)> fn;
    uint64_t calls = 0;
};

struct HostState {
    uint64_t update_calls = 0;
};

int host_update(void *ctx, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t *out_rax) {
    (void)a2;
    (void)a3;
    if (!out_rax) return 1;
    HostState *state = static_cast<HostState *>(ctx);
    state->update_calls++;
    *out_rax = (uint32_t)((uint32_t)a0 * 3u + (uint32_t)a1 + 1u);
    return 0;
}

struct AotInfo {
    void *handle = nullptr;
    AotRunFn run = nullptr;
    uint64_t guest_abi = 0;
    uint64_t artifact_abi = 0;
    uint64_t translator_abi = 0;
    uint64_t guest_state_size = 0;
    const char *target = nullptr;
    const char *module_kind = nullptr;
    uint64_t module_file_size = 0;
    uint64_t preferred_base = 0;
    uint64_t load_base = 0;
    uint64_t image_size = 0;
    const uint8_t *image_sha256 = nullptr;
    const uint8_t *import_binding_sha256 = nullptr;
    const uint8_t *host_binding_sha256 = nullptr;
    const uint8_t *entry_exports_sha256 = nullptr;
    uint64_t thunk_base = 0;
    uint64_t import_count = 0;
    const uint64_t *import_thunks = nullptr;
    uint64_t relocs_stripped = 0;
    const uint8_t *input_sha256 = nullptr;
    const uint64_t *input_hash = nullptr;
    const uint64_t *entry_addr = nullptr;
    const uint8_t *function_sha256 = nullptr;
    const AevBlock *blocks = nullptr;
    const uint64_t *block_count = nullptr;
    const uint64_t *all_count = nullptr;
    const uint64_t *all_blocks = nullptr;
};

struct TraceWriter {
    std::ofstream stream;
    bool open = false;
    uint64_t call_index = 0;
    const aev::Program *program = nullptr;
    const pe::Module *module = nullptr;
    std::string entry_export;
    uint8_t artifact_sha[32] = {};
    std::vector<uint64_t> all_blocks;
    std::vector<uint64_t> aot_selected;

    void write_block(uint32_t source, uint64_t rip) {
        if (!open) return;
        const char *kind = source == AEV_TRACE_SINK_SOURCE_AOT
                               ? "aot"
                               : (source == AEV_TRACE_SINK_SOURCE_JIT_COMPILE
                                      ? "jit_compile"
                                      : "jit_exec");
        aev::BlockIdentity id;
        std::string error;
        stream << "{\"record\":\"" << kind << "\",\"call\":"
               << call_index << ",\"rip\":\"" << hex(rip) << "\"";
        if (aev::block_identity(*program, rip, &id, &error)) {
            stream << ",\"offset\":" << id.section_offset
                   << ",\"rva\":" << id.section_offset
                   << ",\"size\":" << id.size
                   << ",\"block_sha256\":\"" << aev::hex_bytes(id.sha256, 32) << "\"";
        } else {
            stream << ",\"error\":\"" << json_escape(error) << "\"";
        }
        stream << "}\n";
    }

    void host_import(uint64_t id, uint32_t index) {
        if (!open) return;
        stream << "{\"record\":\"host_import\",\"call\":"
               << call_index << ",\"id\":" << id
               << ",\"thunk_index\":" << index << "}\n";
    }

    void dispatch_miss(uint64_t rip, int status) {
        if (!open) return;
        stream << "{\"record\":\"dispatch_miss\",\"call\":"
               << call_index << ",\"rip\":\"" << hex(rip)
               << "\",\"status\":" << status << "}\n";
    }

    void call_end(const GuestState &s, int status) {
        if (!open) return;
        stream << "{\"record\":\"call_end\",\"call\":" << call_index
               << ",\"status\":" << status
               << ",\"error_pc\":\"" << hex(s.error_pc)
               << "\",\"guest_error\":" << s.error_code
               << ",\"mem_fault_kind\":" << s.mem_fault_kind
               << ",\"mem_fault_addr\":\"" << hex(s.mem_fault_addr)
               << "\",\"mem_fault_size\":" << s.mem_fault_size
               << ",\"mem_access\":" << s.mem_access << "}\n";
    }
};

extern "C" void trace_sink_block(void *ctx, uint32_t source, uint64_t rip) {
    static_cast<TraceWriter *>(ctx)->write_block(source, rip);
}

struct ModuleInstance {
    pe::Module module;
    aev::Program program;
    std::unique_ptr<aev::JitRuntime> jit;
    AotInfo aot;
    std::vector<uint64_t> selected_blocks;
    std::map<uint64_t, const pe::Import *> imports_by_thunk;
    std::map<uint64_t, HostImpl> host_by_id;
    HostState host_state;
    TraceWriter trace;
    AevTraceSink trace_sink{};
    std::string entry_export;
    uint8_t artifact_sha[32] = {};

    ~ModuleInstance() {
        jit.reset();
        if (aot.handle) dlclose(aot.handle);
    }
};

struct Options {
    std::string dll;
    std::string registry;
    std::string aot;
    std::string load_base;
    std::string entry_export;
    std::string trace;
    std::string scenario;
    std::string dump_image;
    std::string dump_blocks;
    std::string dump_relocations;
    std::vector<std::string> calls;
    int reload = 1;
    bool info = false;
    bool expect_error = false;
};

struct CallSpec {
    std::string export_name;
    uint64_t args[4] = {0, 0, 0, 0};
};

struct CallResult {
    std::string export_name;
    uint64_t args[4] = {0, 0, 0, 0};
    int status = PE_OK;
    uint64_t rax = 0;
    uint64_t guest_error = 0;
    uint64_t error_pc = 0;
    uint64_t mem_fault_kind = 0;
    uint64_t mem_fault_addr = 0;
    uint64_t mem_fault_size = 0;
    uint64_t mem_access = 0;
    uint64_t aot_blocks = 0;
    uint64_t jit_blocks = 0;
    uint64_t jit_compiles = 0;
    uint64_t jit_execs = 0;
    uint64_t steps = 0;
    uint64_t rip = 0;
    uint64_t rsp = 0;
};

std::string status_json(int status) {
    std::ostringstream os;
    os << "\"status\":" << status << ",\"status_name\":\""
       << status_name(status) << "\"";
    return os.str();
}

bool parse_u64(const std::string &text, uint64_t *out) {
    if (text.empty()) return false;
    char *end = nullptr;
    unsigned long long value = std::strtoull(text.c_str(), &end, 0);
    if (!end || *end != 0) return false;
    *out = (uint64_t)value;
    return true;
}

bool parse_args(const std::string &text, uint64_t args[4]) {
    if (text.empty()) return true;
    size_t start = 0;
    int index = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        std::string part = comma == std::string::npos
                               ? text.substr(start)
                               : text.substr(start, comma - start);
        if (part.empty() || index >= 4) return false;
        char *end = nullptr;
        long long value = std::strtoll(part.c_str(), &end, 0);
        if (!end || *end != 0) return false;
        args[index++] = (uint64_t)value;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return true;
}

bool parse_call(const std::string &text, CallSpec *out) {
    if (!out) return false;
    size_t colon = text.find(':');
    out->export_name = colon == std::string::npos ? text : text.substr(0, colon);
    if (out->export_name.empty()) return false;
    if (colon == std::string::npos) return true;
    return parse_args(text.substr(colon + 1), out->args);
}

void print_module_json(std::ostringstream &os, const ModuleInstance &inst,
                       bool include_blocks) {
    const pe::Module &m = inst.module;
    os << "\"module\":{";
    os << "\"kind\":\"" << AEV_MODULE_KIND_PE64 << "\"";
    os << ",\"file_sha256\":\"" << aev::hex_bytes(m.file_sha256, 32) << "\"";
    os << ",\"file_size\":" << m.file_size;
    os << ",\"file_fnv\":\"" << hex(m.file_fnv) << "\"";
    os << ",\"preferred_base\":\"" << hex(m.preferred_base) << "\"";
    os << ",\"load_base\":\"" << hex(m.load_base) << "\"";
    os << ",\"image_size\":" << m.size_of_image;
    os << ",\"image_sha256\":\"" << aev::hex_bytes(m.image_sha256, 32) << "\"";
    os << ",\"import_binding_sha256\":\""
       << aev::hex_bytes(m.import_binding_sha256, 32) << "\"";
    os << ",\"host_binding_sha256\":\""
       << aev::hex_bytes(m.host_binding_sha256, 32) << "\"";
    os << ",\"entry_exports_sha256\":\""
       << aev::hex_bytes(m.entry_exports_sha256, 32) << "\"";
    os << ",\"thunk_base\":\"" << hex(m.thunk_base) << "\"";
    os << ",\"import_count\":" << m.unique_imports.size();
    os << ",\"relocs_stripped\":" << (m.relocs_stripped ? 1 : 0);
    os << ",\"entry_export\":\"" << json_escape(inst.entry_export) << "\"";
    os << ",\"entry_va\":\"" << hex(m.entry_va) << "\"";
    os << ",\"stack_base\":\"" << hex(m.stack_base) << "\"";
    os << ",\"stack_top\":\"" << hex(m.stack_top) << "\"";
    os << ",\"entry_rsp\":\"" << hex(m.entry_rsp) << "\"";
    os << ",\"relocation_count\":" << m.relocations.size();
    os << ",\"relocation_delta\":\"" << hex(m.load_base - m.preferred_base) << "\"";
    os << ",\"pdata_count\":" << m.pdata.size();
    os << "}";

    os << ",\"sections\":[";
    for (size_t i = 0; i < m.sections.size(); ++i) {
        const auto &s = m.sections[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(s.name)
           << "\",\"rva\":" << s.virtual_address
           << ",\"virtual_size\":" << s.effective_virtual_size
           << ",\"raw_size\":" << s.raw_size
           << ",\"mapped_size\":" << s.mapped_size
           << ",\"prot\":" << ((s.readable ? 1 : 0) | (s.writable ? 2 : 0) |
                               (s.executable ? 4 : 0)) << "}";
    }
    os << "]";

    os << ",\"exports\":[";
    for (size_t i = 0; i < m.exports.size(); ++i) {
        const auto &e = m.exports[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(e.name) << "\",\"rva\":" << e.rva
           << ",\"va\":\"" << hex(m.load_base + e.rva) << "\",\"ordinal\":"
           << e.ordinal << "}";
    }
    os << "]";
    os << ",\"ordinal_only_exports\":[";
    for (size_t i = 0; i < m.ordinal_only_exports.size(); ++i) {
        const auto &e = m.ordinal_only_exports[i];
        if (i) os << ",";
        os << "{\"rva\":" << e.rva << ",\"ordinal\":" << e.ordinal << "}";
    }
    os << "]";

    os << ",\"imports\":[";
    for (size_t i = 0; i < m.unique_imports.size(); ++i) {
        const auto &imp = m.unique_imports[i];
        if (i) os << ",";
        os << "{\"module\":\"" << json_escape(imp.module)
           << "\",\"symbol\":\"" << json_escape(imp.symbol)
           << "\",\"thunk_va\":\"" << hex(imp.thunk_va)
           << "\",\"thunk_index\":" << imp.thunk_index
           << ",\"iat_rva\":" << imp.iat_rva
           << ",\"host_import_id\":" << imp.host_import_id
           << ",\"args\":" << imp.args
           << ",\"abi\":\"" << json_escape(imp.abi) << "\"}";
    }
    os << "]";

    os << ",\"regions\":[";
    for (size_t i = 0; i < m.regions.size(); ++i) {
        const auto &r = m.regions[i];
        if (i) os << ",";
        os << "{\"guest_base\":\"" << hex(r.guest_base)
           << "\",\"size\":" << r.size << ",\"prot\":" << r.prot << "}";
    }
    os << "]";

    os << ",\"aot\":{\"present\":"
       << (inst.aot.handle ? "true" : "false");
    os << ",\"sha256\":\"" << aev::hex_bytes(inst.artifact_sha, 32) << "\"";
    os << ",\"selected_count\":" << inst.selected_blocks.size();
    size_t all_block_count = 0;
    for (const auto &fn : inst.program.functions)
        all_block_count += fn.blocks.size();
    os << ",\"all_count\":" << all_block_count;
    os << "}";

    if (include_blocks) {
        os << ",\"blocks\":[";
        bool first = true;
        std::vector<uint64_t> all;
        for (const auto &fn : inst.program.functions)
            for (const auto &b : fn.blocks)
                all.push_back(b.address);
        std::sort(all.begin(), all.end());
        std::set<uint64_t> selected(inst.selected_blocks.begin(),
                                    inst.selected_blocks.end());
        for (uint64_t rip : all) {
            aev::BlockIdentity id;
            std::string error;
            if (!aev::block_identity(inst.program, rip, &id, &error))
                continue;
            if (!first) os << ",";
            first = false;
            os << "{\"rip\":\"" << hex(rip) << "\",\"rva\":" << id.section_offset
               << ",\"size\":" << id.size
               << ",\"sha256\":\"" << aev::hex_bytes(id.sha256, 32)
               << "\",\"aot_selected\":"
               << (selected.count(rip) ? "true" : "false") << "}";
        }
        os << "]";
    }
}

int map_guest_status(const GuestState &s) {
    switch (s.error_code) {
        case AEV_ERR_NONE: return PE_OK;
        case AEV_ERR_MEMORY: return PE_MEMORY;
        case AEV_ERR_NO_BLOCK:
        case AEV_ERR_BAD_TARGET: return PE_JIT;
        case AEV_ERR_JIT: return PE_JIT;
        case AEV_ERR_STEP_LIMIT: return PE_STEP_LIMIT;
        case AEV_ERR_HOST: return PE_HOST;
        default: return PE_INTERNAL;
    }
}

int64_t dispatch(void *opaque, GuestState *s, uint64_t rip);

bool load_aot_file(ModuleInstance *inst, const std::string &path,
                   std::string *error) {
    AotInfo &a = inst->aot;
    a.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!a.handle) {
        if (error) *error = std::string("dlopen AOT failed: ") + dlerror();
        return false;
    }
    auto sym = [&](const char *name) { return dlsym(a.handle, name); };
    a.run = reinterpret_cast<AotRunFn>(sym("aev_aot_run"));
    auto u64 = [&](const char *name) -> const uint64_t * {
        return static_cast<const uint64_t *>(sym(name));
    };
    auto bytes = [&](const char *name) -> const uint8_t * {
        return static_cast<const uint8_t *>(sym(name));
    };
    a.guest_abi = u64("aev_aot_abi_version") ? *u64("aev_aot_abi_version") : 0;
    a.artifact_abi = u64("aev_artifact_abi_version") ? *u64("aev_artifact_abi_version") : 0;
    a.translator_abi = u64("aev_translator_abi_version") ? *u64("aev_translator_abi_version") : 0;
    a.guest_state_size = u64("aev_guest_state_size") ? *u64("aev_guest_state_size") : 0;
    a.target = static_cast<const char *>(sym("aev_target_triple"));
    a.module_kind = static_cast<const char *>(sym("aev_module_kind"));
    a.module_file_size = u64("aev_module_file_size") ? *u64("aev_module_file_size") : 0;
    a.preferred_base = u64("aev_preferred_base") ? *u64("aev_preferred_base") : 0;
    a.load_base = u64("aev_load_base") ? *u64("aev_load_base") : 0;
    a.image_size = u64("aev_image_size") ? *u64("aev_image_size") : 0;
    a.image_sha256 = bytes("aev_image_sha256");
    a.import_binding_sha256 = bytes("aev_import_binding_sha256");
    a.host_binding_sha256 = bytes("aev_host_binding_sha256");
    a.entry_exports_sha256 = bytes("aev_entry_exports_sha256");
    a.thunk_base = u64("aev_thunk_base") ? *u64("aev_thunk_base") : 0;
    a.import_count = u64("aev_import_count") ? *u64("aev_import_count") : 0;
    a.import_thunks = u64("aev_import_thunks");
    a.relocs_stripped = u64("aev_relocs_stripped") ? *u64("aev_relocs_stripped") : 0;
    a.input_sha256 = bytes("aev_input_sha256");
    a.input_hash = u64("aev_input_hash");
    a.entry_addr = u64("aev_entry_addr");
    a.function_sha256 = bytes("aev_function_name_sha256");
    a.blocks = static_cast<const AevBlock *>(sym("aev_blocks"));
    a.block_count = u64("aev_block_count");
    a.all_count = u64("aev_all_block_count");
    a.all_blocks = u64("aev_all_blocks");
    if (!a.run || !a.input_hash || !a.entry_addr || !a.blocks || !a.block_count ||
        !a.all_count || !a.all_blocks || !a.target || !a.module_kind ||
        !a.image_sha256 || !a.import_binding_sha256 || !a.host_binding_sha256 ||
        !a.entry_exports_sha256 || !a.input_sha256 || !a.function_sha256 ||
        !a.image_sha256) {
        if (error) *error = "AOT dylib missing required symbols";
        return false;
    }
    if (a.guest_abi != AEV_GUEST_ABI_VERSION ||
        a.guest_state_size != sizeof(GuestState) ||
        a.artifact_abi != AEV_ARTIFACT_ABI_VERSION ||
        a.translator_abi != AEV_TRANSLATOR_ABI_VERSION) {
        if (error) *error = "AOT ABI/guest-state/artifact/translator mismatch";
        return false;
    }
    if (std::string(a.module_kind) != AEV_MODULE_KIND_PE64) {
        if (error) *error = std::string("AOT module kind is not PE: ") + a.module_kind;
        return false;
    }
    if (std::string(a.target) != AEV_AOT_TARGET_TRIPLE) {
        if (error) *error = std::string("AOT target mismatch: ") + a.target;
        return false;
    }
    const pe::Module &m = inst->module;
    if (a.module_file_size != m.file_size ||
        a.preferred_base != m.preferred_base ||
        a.load_base != m.load_base ||
        a.image_size != m.size_of_image ||
        std::memcmp(a.image_sha256, m.image_sha256, 32) != 0 ||
        std::memcmp(a.import_binding_sha256, m.import_binding_sha256, 32) != 0 ||
        std::memcmp(a.host_binding_sha256, m.host_binding_sha256, 32) != 0 ||
        std::memcmp(a.entry_exports_sha256, m.entry_exports_sha256, 32) != 0 ||
        a.thunk_base != m.thunk_base ||
        a.import_count != m.unique_imports.size() ||
        a.relocs_stripped != (m.relocs_stripped ? 1 : 0)) {
        if (error) *error = "AOT module identity does not match loaded PE module";
        return false;
    }
    if (std::memcmp(a.input_sha256, m.file_sha256, 32) != 0 ||
        *a.input_hash != m.file_fnv || *a.entry_addr != m.entry_va) {
        if (error) *error = "AOT input/entry identity mismatch";
        return false;
    }
    uint8_t function_sha[32];
    aev::sha256(inst->entry_export.data(), inst->entry_export.size(), function_sha);
    if (std::memcmp(a.function_sha256, function_sha, 32) != 0) {
        if (error) *error = "AOT entry export hash mismatch";
        return false;
    }
    if (a.import_count > 256 || (a.import_count > 0 && !a.import_thunks)) {
        if (error) *error = "AOT import table is invalid";
        return false;
    }
    for (size_t i = 0; i < m.unique_imports.size(); ++i) {
        if (a.import_thunks[i] != m.unique_imports[i].thunk_va) {
            if (error) *error = "AOT import thunk table mismatch";
            return false;
        }
    }

    std::vector<uint64_t> program_blocks;
    for (const auto &fn : inst->program.functions)
        for (const auto &b : fn.blocks)
            program_blocks.push_back(b.address);
    std::sort(program_blocks.begin(), program_blocks.end());
    if (*a.all_count != program_blocks.size()) {
        if (error) *error = "AOT all-block count does not match program";
        return false;
    }
    for (size_t i = 0; i < program_blocks.size(); ++i) {
        if (a.all_blocks[i] != program_blocks[i]) {
            if (error) *error = "AOT all-block table does not match program";
            return false;
        }
    }
    if (*a.block_count > program_blocks.size()) {
        if (error) *error = "AOT selected count exceeds all blocks";
        return false;
    }
    std::set<uint64_t> selected;
    for (uint64_t i = 0; i < *a.block_count; ++i) {
        uint64_t rip = a.blocks[i].rip;
        if (!a.blocks[i].fn ||
            !std::binary_search(program_blocks.begin(), program_blocks.end(), rip) ||
            (i > 0 && a.blocks[i - 1].rip >= rip) ||
            !selected.insert(rip).second) {
            if (error) *error = "AOT selected block table is invalid";
            return false;
        }
        char symbol[64];
        std::snprintf(symbol, sizeof(symbol), "aev_blk_%llx",
                      (unsigned long long)rip);
        void *expected = dlsym(a.handle, symbol);
        if (!expected ||
            reinterpret_cast<void *>(a.blocks[i].fn) != expected) {
            if (error) *error = "AOT selected block fn/symbol mismatch";
            return false;
        }
    }
    inst->selected_blocks.assign(selected.begin(), selected.end());
    aev::sha256_file(path, inst->artifact_sha, error);
    return true;
}

bool peek_aot_load_base(const std::string &path, uint64_t *out,
                         std::string *error) {
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        if (error) *error = std::string("dlopen AOT failed: ") + dlerror();
        return false;
    }
    const uint64_t *load_base =
        static_cast<const uint64_t *>(dlsym(handle, "aev_load_base"));
    bool ok = load_base != nullptr;
    if (ok && out) *out = *load_base;
    if (!ok && error) *error = "AOT dylib missing aev_load_base";
    dlclose(handle);
    return ok;
}

bool prepare_instance(const Options &opt, ModuleInstance *inst,
                      std::string *error) {
    aev::pe::ImportRegistry registry;
    if (!registry.load(opt.registry, error)) return false;
    uint64_t load_base = 0;
    if (!opt.load_base.empty() && !parse_u64(opt.load_base, &load_base)) {
        if (error) *error = "invalid --load-base";
        return false;
    }
    if (load_base == 0 && opt.aot.empty()) {
        if (error) *error = "--load-base is required without --aot";
        return false;
    }
    if (load_base == 0 && !opt.aot.empty()) {
        if (!peek_aot_load_base(opt.aot, &load_base, error)) return false;
    }
    aev::pe::LoadOptions load_options;
    load_options.load_base = load_base;
    if (!aev::pe::load_module(opt.dll, load_options, registry, &inst->module, error))
        return false;
    inst->entry_export = opt.entry_export;
    if (inst->entry_export.empty()) {
        if (inst->module.exports.empty()) {
            if (error) *error = "module has no exports";
            return false;
        }
        inst->entry_export = inst->module.exports.front().name;
    }
    const pe::Export *entry = inst->module.find_export(inst->entry_export);
    if (!entry) {
        if (error) *error = "entry export not found: " + inst->entry_export;
        return false;
    }
    inst->module.primary_export = inst->entry_export;
    inst->module.entry_va = inst->module.load_base + entry->rva;

    std::vector<std::string> roots;
    for (const auto &e : inst->module.exports)
        roots.push_back(e.name);
    std::vector<aev::FunctionSpec> specs;
    if (!pe::discover_functions(inst->module, roots, &specs, error)) return false;
    if (!pe::build_program_for_module(inst->module, specs, &inst->program, error))
        return false;
    for (auto &fn : inst->program.functions) {
        for (auto &b : fn.blocks) {
            b.jit_only = true;
            b.jit_reason = "no AOT artifact (JIT-only run)";
        }
    }

    for (const auto &imp : inst->module.unique_imports) {
        if (imp.host_import_id == 1) {
            HostImpl impl;
            impl.id = 1;
            impl.args = 2;
            impl.ctx = &inst->host_state;
            void *ctx = impl.ctx;
            impl.fn = [ctx](uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t *out) {
                return host_update(ctx, a0, a1, a2, a3, out);
            };
            inst->host_by_id[1] = impl;
        } else {
            if (error) *error = "no host implementation for import id " +
                                 std::to_string(imp.host_import_id);
            return false;
        }
        inst->imports_by_thunk[imp.thunk_va] = &imp;
    }

    if (!opt.aot.empty()) {
        if (!load_aot_file(inst, opt.aot, error)) return false;
        if (inst->module.load_base != inst->aot.load_base) {
            if (error) *error = "artifact load_base does not match requested load_base";
            return false;
        }
        std::set<uint64_t> selected(inst->selected_blocks.begin(),
                                    inst->selected_blocks.end());
        for (auto &fn : inst->program.functions) {
            for (auto &b : fn.blocks) {
                if (selected.count(b.address)) {
                    b.jit_only = false;
                    b.jit_reason.clear();
                } else {
                    b.jit_only = true;
                    b.jit_reason = "not selected in AOT coverage";
                }
            }
        }
    } else {
        inst->artifact_sha[0] = 0;
    }

    inst->jit = std::make_unique<aev::JitRuntime>(&inst->program);
    if (!inst->jit->init(error)) return false;
    if (!opt.trace.empty()) {
        inst->trace.program = &inst->program;
        inst->trace.module = &inst->module;
        inst->trace.entry_export = inst->entry_export;
        std::memcpy(inst->trace.artifact_sha, inst->artifact_sha, 32);
        inst->trace.aot_selected = inst->selected_blocks;
        for (const auto &fn : inst->program.functions)
            for (const auto &b : fn.blocks)
                inst->trace.all_blocks.push_back(b.address);
        std::sort(inst->trace.all_blocks.begin(), inst->trace.all_blocks.end());
        inst->trace.stream.open(opt.trace, std::ios::out | std::ios::trunc);
        if (!inst->trace.stream) {
            if (error) *error = "cannot open trace file";
            return false;
        }
        uint8_t function_sha[32];
        aev::sha256(inst->entry_export.data(), inst->entry_export.size(), function_sha);
        std::ofstream &out = inst->trace.stream;
        out << "{\"record\":\"header\",\"trace_abi\":" << AEV_TRACE_FILE_ABI_PE
            << ",\"scenario\":\""
            << json_escape(opt.scenario.empty() ? std::string("A") : opt.scenario)
            << "\",\"module_kind\":\"" << AEV_MODULE_KIND_PE64 << "\""
            << ",\"input_sha256\":\"" << aev::hex_bytes(inst->module.file_sha256, 32)
            << "\",\"input_fnv\":\"" << hex(inst->module.file_fnv) << "\""
            << ",\"module_file_size\":" << inst->module.file_size
            << ",\"preferred_base\":\"" << hex(inst->module.preferred_base) << "\""
            << ",\"load_base\":\"" << hex(inst->module.load_base) << "\""
            << ",\"image_size\":" << inst->module.size_of_image
            << ",\"image_sha256\":\"" << aev::hex_bytes(inst->module.image_sha256, 32)
            << "\",\"import_binding_sha256\":\""
            << aev::hex_bytes(inst->module.import_binding_sha256, 32)
            << "\",\"host_binding_sha256\":\""
            << aev::hex_bytes(inst->module.host_binding_sha256, 32)
            << "\",\"entry_exports_sha256\":\""
            << aev::hex_bytes(inst->module.entry_exports_sha256, 32)
            << "\",\"entry_exports\":[";
        for (size_t i = 0; i < inst->module.exports.size(); ++i) {
            if (i) out << ",";
            out << "\"" << json_escape(inst->module.exports[i].name) << "\"";
        }
        out << "],\"entry\":\"" << json_escape(inst->entry_export) << "\""
            << ",\"function\":\"" << json_escape(inst->entry_export) << "\""
            << ",\"entry_addr\":\"" << hex(inst->module.entry_va) << "\""
            << ",\"thunk_base\":\"" << hex(inst->module.thunk_base) << "\""
            << ",\"import_count\":" << inst->module.unique_imports.size()
            << ",\"entry_function_sha256\":\""
            << aev::hex_bytes(function_sha, 32) << "\""
            << ",\"offset_kind\":\"rva\""
            << ",\"guest_abi\":2,\"aot_artifact_abi\":2,\"translator_abi\":2"
            << ",\"callback_abi\":3"
            << ",\"target\":\"" << AEV_AOT_TARGET_TRIPLE << "\""
            << ",\"translator_version\":\"aev-translate-v2\""
            << ",\"trace_tool_version\":\"aev-pe-trace-v1\""
            << ",\"aot_sha256\":\""
            << aev::hex_bytes(inst->artifact_sha, 32) << "\""
            << ",\"all_blocks\":[";
        for (size_t i = 0; i < inst->trace.all_blocks.size(); ++i) {
            if (i) out << ",";
            out << "\"" << hex(inst->trace.all_blocks[i]) << "\"";
        }
        out << "],\"aot_selected\":[";
        for (size_t i = 0; i < inst->selected_blocks.size(); ++i) {
            if (i) out << ",";
            out << "\"" << hex(inst->selected_blocks[i]) << "\"";
        }
        out << "]}\n";
        inst->trace.open = true;
        inst->trace_sink.abi_version = AEV_TRACE_SINK_ABI_VERSION;
        inst->trace_sink.on_block = trace_sink_block;
        inst->trace_sink.ctx = &inst->trace;
        inst->jit->set_trace_hook(&inst->trace, [](void *ctx, uint32_t source,
                                                   uint64_t rip) {
            static_cast<TraceWriter *>(ctx)->write_block(source, rip);
        });
    }
    return true;
}

int64_t dispatch(void *opaque, GuestState *s, uint64_t rip) {
    ModuleInstance *inst = static_cast<ModuleInstance *>(opaque);
    const pe::Module &m = inst->module;
    uint64_t thunk_end = m.thunk_base + m.unique_imports.size() * pe::kThunkStride;
    if (rip >= m.thunk_base && rip < thunk_end) {
        auto it = inst->imports_by_thunk.find(rip);
        if (it == inst->imports_by_thunk.end()) {
            s->error_code = AEV_ERR_BAD_TARGET;
            s->error_pc = rip;
            inst->trace.dispatch_miss(rip, PE_UNKNOWN_IMPORT);
            return -1;
        }
        if (s->pending_transfer != AEV_TRANSFER_CALL) {
            s->error_code = AEV_ERR_BAD_TARGET;
            s->error_pc = rip;
            inst->trace.dispatch_miss(rip, PE_JIT);
            return -1;
        }
        uint64_t ret = aev_load(s, s->rsp, 8, 0);
        if (s->error_code) {
            inst->trace.dispatch_miss(rip, PE_MEMORY);
            return -1;
        }
        s->rsp += 8;
        auto hit = inst->host_by_id.find(it->second->host_import_id);
        if (hit == inst->host_by_id.end()) {
            s->error_code = AEV_ERR_HOST;
            s->error_pc = rip;
            inst->trace.dispatch_miss(rip, PE_UNKNOWN_IMPORT);
            return -1;
        }
        uint64_t out_rax = 0;
        int status = hit->second.fn(s->rcx, s->rdx, s->r8, s->r9, &out_rax);
        hit->second.calls++;
        inst->trace.host_import(it->second->host_import_id,
                                it->second->thunk_index);
        if (status != 0) {
            s->error_code = AEV_ERR_HOST;
            s->error_pc = rip;
            return -1;
        }
        s->rax = out_rax;
        s->rip = ret;
        s->pending_transfer = AEV_TRANSFER_NONE;
        return 0;
    }
    int64_t rc = inst->jit->dispatch(s, rip);
    if (rc != 0) {
        if (!s->error_code) s->error_code = AEV_ERR_JIT;
        inst->trace.dispatch_miss(rip, map_guest_status(*s));
    }
    return rc;
}

bool write_guest(GuestState *s, uint64_t addr, const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; i += 8) {
        uint64_t value = 0;
        size_t chunk = std::min<size_t>(8, len - i);
        std::memcpy(&value, p + i, chunk);
        aev_store(s, addr + i, value, (uint32_t)chunk);
        if (s->error_code) return false;
    }
    return true;
}

CallResult run_call(ModuleInstance *inst, const CallSpec &spec) {
    CallResult result;
    result.export_name = spec.export_name;
    std::memcpy(result.args, spec.args, sizeof(result.args));
    const pe::Export *e = inst->module.find_export(spec.export_name);
    if (!e) {
        result.status = PE_INVALID_ARGUMENT;
        return result;
    }
    const aev::GuestFunction *fn = inst->program.find_function_by_name(spec.export_name);
    if (!fn) {
        result.status = PE_INVALID_ARGUMENT;
        return result;
    }
    GuestState state{};
    state.reserved1 = (uint64_t)(uintptr_t)inst->module.regions.data();
    state.reserved2 = inst->module.regions.size();
    state.reserved3 = inst->module.load_base;
    state.rsp = inst->module.entry_rsp;
    state.rip = inst->module.load_base + e->rva;
    state.rcx = spec.args[0];
    state.rdx = spec.args[1];
    state.r8 = spec.args[2];
    state.r9 = spec.args[3];
    state.reserved0 = inst->trace.open
                          ? (uint64_t)(uintptr_t)&inst->trace_sink
                          : 0;
    uint64_t sentinel = AEV_RETURN_SENTINEL;
    if (!write_guest(&state, state.rsp, &sentinel, 8)) {
        result.status = map_guest_status(state);
    } else {
        uint8_t shadow[0x20] = {};
        if (!write_guest(&state, state.rsp + 8, shadow, sizeof(shadow)))
            result.status = map_guest_status(state);
        else {
            inst->trace.call_index++;
            AevRuntime rt;
            rt.jit_dispatch = dispatch;
            rt.ctx = inst;
            int rc = 0;
            if (inst->aot.run) {
                rc = inst->aot.run(&state, &rt);
            } else {
                const uint64_t limit = 1000000;
                for (uint64_t steps = 0; steps < limit; ++steps) {
                    if (state.error_code) {
                        rc = (int)state.error_code;
                        break;
                    }
                    if (state.rip == AEV_RETURN_SENTINEL) {
                        rc = 0;
                        break;
                    }
                    state.steps = steps + 1;
                    int64_t d = dispatch(inst, &state, state.rip);
                    if (d != 0) {
                        rc = state.error_code ? (int)state.error_code : (int)AEV_ERR_JIT;
                        break;
                    }
                }
                if (rc == 0 && state.rip != AEV_RETURN_SENTINEL)
                    rc = (int)AEV_ERR_STEP_LIMIT;
            }
            if (rc == 0 &&
                (!(state.rip == AEV_RETURN_SENTINEL &&
                   state.rsp == inst->module.entry_rsp + 8 && state.error_code == 0)))
                result.status = PE_INTERNAL;
            else
                result.status = rc == 0 ? PE_OK : map_guest_status(state);
        }
    }
    inst->trace.call_end(state, result.status);
    result.rax = state.rax;
    result.guest_error = state.error_code;
    result.error_pc = state.error_pc;
    result.mem_fault_kind = state.mem_fault_kind;
    result.mem_fault_addr = state.mem_fault_addr;
    result.mem_fault_size = state.mem_fault_size;
    result.mem_access = state.mem_access;
    result.aot_blocks = state.aot_blocks;
    result.jit_blocks = state.jit_blocks;
    result.jit_compiles = state.jit_compiles;
    result.jit_execs = state.jit_execs;
    result.steps = state.steps;
    result.rip = state.rip;
    result.rsp = state.rsp;
    state.reserved0 = 0;
    return result;
}

void print_result_json(std::ostringstream &os, const CallResult &r) {
    os << "{\"export\":\"" << json_escape(r.export_name) << "\""
       << ",\"args\":[" << r.args[0] << "," << r.args[1] << "," << r.args[2]
       << "," << r.args[3] << "]"
       << "," << status_json(r.status)
       << ",\"rax\":" << (int64_t)r.rax
       << ",\"guest_error\":" << r.guest_error
       << ",\"error_pc\":\"" << hex(r.error_pc) << "\""
       << ",\"mem_fault_kind\":" << r.mem_fault_kind
       << ",\"mem_fault_addr\":\"" << hex(r.mem_fault_addr) << "\""
       << ",\"mem_fault_size\":" << r.mem_fault_size
       << ",\"mem_access\":" << r.mem_access
       << ",\"aot_blocks\":" << r.aot_blocks
       << ",\"jit_blocks\":" << r.jit_blocks
       << ",\"jit_compiles\":" << r.jit_compiles
       << ",\"jit_execs\":" << r.jit_execs
       << ",\"steps\":" << r.steps
       << ",\"rip\":\"" << hex(r.rip) << "\""
       << ",\"rsp\":\"" << hex(r.rsp) << "\"}";
}

struct HandleTable {
    std::array<std::unique_ptr<ModuleInstance>, 8> slots;
    std::array<uint32_t, 8> generations{};
    uint64_t load(std::unique_ptr<ModuleInstance> instance) {
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i]) {
                if (generations[i] == 0) generations[i] = 1;
                slots[i] = std::move(instance);
                return (uint64_t)(i + 1) | ((uint64_t)generations[i] << 32);
            }
        }
        return 0;
    }
    ModuleInstance *get(uint64_t token) {
        uint32_t slot_plus = (uint32_t)(token & 0xffffffffULL);
        uint32_t gen = (uint32_t)(token >> 32);
        if (slot_plus == 0 || slot_plus > slots.size()) return nullptr;
        size_t i = slot_plus - 1;
        if (!slots[i] || generations[i] != gen) return nullptr;
        return slots[i].get();
    }
    int destroy(uint64_t token) {
        uint32_t slot_plus = (uint32_t)(token & 0xffffffffULL);
        uint32_t gen = (uint32_t)(token >> 32);
        if (slot_plus == 0 || slot_plus > slots.size()) return PE_STALE_HANDLE;
        size_t i = slot_plus - 1;
        if (!slots[i] || generations[i] != gen) return PE_STALE_HANDLE;
        slots[i].reset();
        ++generations[i];
        if (generations[i] == 0) generations[i] = 1;
        return PE_OK;
    }
};

void usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s --dll <path> --registry <path> [--load-base <hex>]\n"
                 "  [--aot <dylib>] [--entry-export <name>]\n"
                 "  [--call <export>[:a0,a1,a2,a3]]... [--reload N]\n"
                 "  [--expect-error] [--trace <path>] [--scenario <name>]\n"
                 "  [--info] [--dump-image <path>] [--dump-blocks <path>]\n"
                 "  [--dump-relocations <path>]\n",
                 argv0);
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *what) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if (a == "--dll") opt.dll = need("--dll");
        else if (a == "--registry") opt.registry = need("--registry");
        else if (a == "--aot") opt.aot = need("--aot");
        else if (a == "--load-base") opt.load_base = need("--load-base");
        else if (a == "--entry-export") opt.entry_export = need("--entry-export");
        else if (a == "--call") opt.calls.push_back(need("--call"));
        else if (a == "--reload") opt.reload = std::max(1, std::atoi(need("--reload").c_str()));
        else if (a == "--trace") opt.trace = need("--trace");
        else if (a == "--scenario") opt.scenario = need("--scenario");
        else if (a == "--dump-image") opt.dump_image = need("--dump-image");
        else if (a == "--dump-blocks") opt.dump_blocks = need("--dump-blocks");
        else if (a == "--dump-relocations") opt.dump_relocations = need("--dump-relocations");
        else if (a == "--info") opt.info = true;
        else if (a == "--expect-error") opt.expect_error = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (opt.dll.empty() || opt.registry.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (!opt.info && opt.calls.empty()) {
        std::fprintf(stderr, "at least one --call is required unless --info\n");
        return 2;
    }
    if (opt.aot.empty() && opt.load_base.empty()) {
        std::fprintf(stderr, "--load-base is required without --aot\n");
        return 2;
    }
    if (!opt.aot.empty() && opt.entry_export.empty()) {
        std::fprintf(stderr, "--entry-export is required with --aot\n");
        return 2;
    }
    std::vector<CallSpec> calls;
    for (const auto &text : opt.calls) {
        CallSpec spec;
        if (!parse_call(text, &spec)) {
            std::fprintf(stderr, "invalid --call: %s\n", text.c_str());
            return 2;
        }
        calls.push_back(spec);
    }

    HandleTable table;
    std::ostringstream out;
    out << "{";
    bool first_reload = true;
    uint64_t first_token = 0;
    int first_failure = PE_OK;
    bool any_failure = false;
    for (int iteration = 0; iteration < opt.reload; ++iteration) {
        auto instance = std::make_unique<ModuleInstance>();
        std::string error;
        if (!prepare_instance(opt, instance.get(), &error)) {
            out << status_json(PE_LOAD) << ",\"error\":\"" << json_escape(error) << "\""
                << ",\"expected_error\":" << (opt.expect_error ? "true" : "false")
                << ",\"any_failure\":true,\"failure_status\":" << PE_LOAD;
            std::cout << out.str() << "}\n";
            return opt.expect_error ? 0 : PE_LOAD;
        }
        uint64_t token = table.load(std::move(instance));
        if (token == 0) {
            out << status_json(PE_INVALID_ARGUMENT) << ",\"error\":\"handle table full\""
                << ",\"expected_error\":" << (opt.expect_error ? "true" : "false")
                << ",\"any_failure\":true,\"failure_status\":" << PE_INVALID_ARGUMENT;
            std::cout << out.str() << "}\n";
            return opt.expect_error ? 0 : PE_INVALID_ARGUMENT;
        }
        if (first_token == 0) first_token = token;
        ModuleInstance *inst = table.get(token);
        if (iteration > 0) out << ",";
        out << "\"iteration_" << iteration << "\":{";
        print_module_json(out, *inst, opt.info);
        if (!opt.trace.empty()) {
            inst->trace.call_index = (uint64_t)iteration * 100000;
        }
        std::vector<CallResult> results;
        bool failed = false;
        for (const auto &spec : calls) {
            CallResult r = run_call(inst, spec);
            results.push_back(r);
            if (r.status != PE_OK) {
                failed = true;
                if (first_failure == PE_OK) first_failure = r.status;
            }
        }
        out << ",\"calls\":[";
        for (size_t i = 0; i < results.size(); ++i) {
            if (i) out << ",";
            print_result_json(out, results[i]);
        }
        out << "]";
        out << ",\"host_import_calls\":"
            << inst->host_state.update_calls;
        out << "}";
        if (first_reload) first_reload = false;
        if (failed) any_failure = true;
        if (opt.dump_image == "" && opt.dump_blocks == "" &&
            opt.dump_relocations == "" && opt.trace.empty() && opt.info == false) {
            // no side effects
        }
        if (!opt.dump_image.empty()) {
            std::ofstream f(opt.dump_image, std::ios::binary);
            f.write(reinterpret_cast<const char *>(inst->module.image.data()),
                    (std::streamsize)inst->module.image.size());
        }
        if (!opt.dump_blocks.empty()) {
            std::ofstream f(opt.dump_blocks);
            f << "[";
            bool first = true;
            std::set<uint64_t> sel(inst->selected_blocks.begin(),
                                   inst->selected_blocks.end());
            for (const auto &fn : inst->program.functions) {
                for (const auto &b : fn.blocks) {
                    aev::BlockIdentity id;
                    std::string e;
                    if (!aev::block_identity(inst->program, b.address, &id, &e))
                        continue;
                    if (!first) f << ",";
                    first = false;
                    f << "{\"rip\":\"" << hex(b.address) << "\",\"rva\":"
                      << id.section_offset << ",\"size\":" << id.size
                      << ",\"sha256\":\"" << aev::hex_bytes(id.sha256, 32)
                      << "\",\"aot_selected\":"
                      << (sel.count(b.address) ? "true" : "false") << "}";
                }
            }
            f << "]\n";
        }
        if (!opt.dump_relocations.empty()) {
            std::ofstream f(opt.dump_relocations);
            f << "{\"count\":" << inst->module.relocations.size()
              << ",\"delta\":" << (inst->module.load_base - inst->module.preferred_base)
              << ",\"stripped\":" << (inst->module.relocs_stripped ? 1 : 0)
              << ",\"sites\":[";
            for (size_t i = 0; i < inst->module.relocations.size(); ++i) {
                if (i) f << ",";
                f << "{\"rva\":" << inst->module.relocations[i].rva
                  << ",\"type\":" << inst->module.relocations[i].type
                  << ",\"va\":\"" << hex(inst->module.load_base +
                                          inst->module.relocations[i].rva)
                  << "\"}";
            }
            f << "]}\n";
        }
        if (table.destroy(token) != PE_OK) {
            any_failure = true;
            if (first_failure == PE_OK) first_failure = PE_STALE_HANDLE;
        }
        if (iteration == 0 && opt.reload > 1) {
            // Expose the stale-token check after the first destroy.
            ModuleInstance *stale = table.get(first_token);
            out << ",\"stale_handle_status\":"
                << (stale ? PE_OK : PE_STALE_HANDLE);
        }
    }
    int final_status = any_failure ? first_failure : PE_OK;
    out << ",\"reload_iterations\":" << opt.reload;
    out << ",\"expected_error\":" << (opt.expect_error ? "true" : "false");
    out << ",\"any_failure\":" << (any_failure ? "true" : "false");
    out << ",\"failure_status\":" << first_failure;
    out << ",\"status\":" << final_status << ",\"status_name\":\""
        << status_name(final_status) << "\"";
    out << "}\n";
    std::cout << out.str();
    if (opt.expect_error) {
        return any_failure ? 0 : 1;
    }
    return final_status;
}
