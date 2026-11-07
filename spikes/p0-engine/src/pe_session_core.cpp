#include "pe_session_core.hpp"

#include "aev_artifact.h"
#include "jit_runtime.hpp"
#include "program_identity.hpp"
#include "sha256.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <utility>

namespace aev::pe {

namespace {

using AotRunFn = int (*)(GuestState *, const AevRuntime *);

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

std::string hex(uint64_t value) { return aev::hex64(value); }

struct HostImpl {
    uint64_t id = 0;
    uint32_t args = 0;
    void *ctx = nullptr;
    std::function<int(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *)> fn;
};

int host_update(void *ctx, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t *out_rax) {
    (void)ctx;
    (void)a2;
    (void)a3;
    if (!out_rax) return 1;
    /* Fixed registry-declared aevhost.dll!AevUpdate(id=1, args=2). */
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
    bool active = false;
    uint64_t call_index = 0;
    uint64_t call_serial = 0;
    uint64_t records = 0;
    const Program *program = nullptr;
    const Module *module = nullptr;
    std::string entry_export;
    uint8_t artifact_sha[32] = {};
    std::vector<uint64_t> all_blocks;
    std::vector<uint64_t> aot_selected;

    void write_block(uint32_t source, uint64_t rip) {
        if (!active) return;
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
                   << ",\"block_sha256\":\"" << aev::hex_bytes(id.sha256, 32)
                   << "\"";
        } else {
            stream << ",\"error\":\"" << json_escape(error) << "\"";
        }
        stream << "}\n";
        ++records;
    }

    void host_import(uint64_t id, uint32_t index) {
        if (!active) return;
        stream << "{\"record\":\"host_import\",\"call\":"
               << call_index << ",\"id\":" << id
               << ",\"thunk_index\":" << index << "}\n";
        ++records;
    }

    void dispatch_miss(uint64_t rip, int status) {
        if (!active) return;
        stream << "{\"record\":\"dispatch_miss\",\"call\":"
               << call_index << ",\"rip\":\"" << hex(rip)
               << "\",\"status\":" << status << "}\n";
        ++records;
    }

    void call_end(const GuestState &s, int status) {
        if (!active) return;
        stream << "{\"record\":\"call_end\",\"call\":" << call_index
               << ",\"status\":" << status
               << ",\"error_pc\":\"" << hex(s.error_pc)
               << "\",\"guest_error\":" << s.error_code
               << ",\"mem_fault_kind\":" << s.mem_fault_kind
               << ",\"mem_fault_addr\":\"" << hex(s.mem_fault_addr)
               << "\",\"mem_fault_size\":" << s.mem_fault_size
               << ",\"mem_access\":" << s.mem_access << "}\n";
        ++records;
    }

    void close() {
        if (stream.is_open()) {
            stream.flush();
            stream.close();
        }
        active = false;
    }
};

extern "C" void trace_sink_block(void *ctx, uint32_t source, uint64_t rip) {
    static_cast<TraceWriter *>(ctx)->write_block(source, rip);
}

int map_guest_status(const GuestState &s) {
    switch (s.error_code) {
        case AEV_ERR_NONE: return (int)SessionStatus::Ok;
        case AEV_ERR_MEMORY: return (int)SessionStatus::Memory;
        case AEV_ERR_NO_BLOCK:
        case AEV_ERR_BAD_TARGET: return (int)SessionStatus::Jit;
        case AEV_ERR_JIT: return (int)SessionStatus::Jit;
        case AEV_ERR_STEP_LIMIT: return (int)SessionStatus::StepLimit;
        case AEV_ERR_HOST: return (int)SessionStatus::Host;
        default: return (int)SessionStatus::Internal;
    }
}

bool load_aot_file(Module *module, const std::string &entry_export,
                   const std::vector<uint64_t> &all_blocks, AotInfo *a,
                   std::vector<uint64_t> *selected_blocks,
                   uint8_t artifact_sha[32], const std::string &path,
                   std::string *error) {
    a->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!a->handle) {
        if (error) *error = std::string("dlopen AOT failed: ") + dlerror();
        return false;
    }
    auto sym = [&](const char *name) { return dlsym(a->handle, name); };
    a->run = reinterpret_cast<AotRunFn>(sym("aev_aot_run"));
    auto u64 = [&](const char *name) -> const uint64_t * {
        return static_cast<const uint64_t *>(sym(name));
    };
    auto bytes = [&](const char *name) -> const uint8_t * {
        return static_cast<const uint8_t *>(sym(name));
    };
    a->guest_abi = u64("aev_aot_abi_version") ? *u64("aev_aot_abi_version") : 0;
    a->artifact_abi = u64("aev_artifact_abi_version")
                          ? *u64("aev_artifact_abi_version")
                          : 0;
    a->translator_abi = u64("aev_translator_abi_version")
                            ? *u64("aev_translator_abi_version")
                            : 0;
    a->guest_state_size = u64("aev_guest_state_size")
                              ? *u64("aev_guest_state_size")
                              : 0;
    a->target = static_cast<const char *>(sym("aev_target_triple"));
    a->module_kind = static_cast<const char *>(sym("aev_module_kind"));
    a->module_file_size = u64("aev_module_file_size")
                              ? *u64("aev_module_file_size")
                              : 0;
    a->preferred_base = u64("aev_preferred_base") ? *u64("aev_preferred_base") : 0;
    a->load_base = u64("aev_load_base") ? *u64("aev_load_base") : 0;
    a->image_size = u64("aev_image_size") ? *u64("aev_image_size") : 0;
    a->image_sha256 = bytes("aev_image_sha256");
    a->import_binding_sha256 = bytes("aev_import_binding_sha256");
    a->host_binding_sha256 = bytes("aev_host_binding_sha256");
    a->entry_exports_sha256 = bytes("aev_entry_exports_sha256");
    a->thunk_base = u64("aev_thunk_base") ? *u64("aev_thunk_base") : 0;
    a->import_count = u64("aev_import_count") ? *u64("aev_import_count") : 0;
    a->import_thunks = u64("aev_import_thunks");
    a->relocs_stripped = u64("aev_relocs_stripped")
                             ? *u64("aev_relocs_stripped")
                             : 0;
    a->input_sha256 = bytes("aev_input_sha256");
    a->input_hash = u64("aev_input_hash");
    a->entry_addr = u64("aev_entry_addr");
    a->function_sha256 = bytes("aev_function_name_sha256");
    a->blocks = static_cast<const AevBlock *>(sym("aev_blocks"));
    a->block_count = u64("aev_block_count");
    a->all_count = u64("aev_all_block_count");
    a->all_blocks = u64("aev_all_blocks");
    if (!a->run || !a->input_hash || !a->entry_addr || !a->blocks ||
        !a->block_count || !a->all_count || !a->all_blocks || !a->target ||
        !a->module_kind || !a->image_sha256 || !a->import_binding_sha256 ||
        !a->host_binding_sha256 || !a->entry_exports_sha256 ||
        !a->input_sha256 || !a->function_sha256) {
        if (error) *error = "AOT dylib missing required symbols";
        return false;
    }
    if (a->guest_abi != AEV_GUEST_ABI_VERSION ||
        a->guest_state_size != sizeof(GuestState) ||
        a->artifact_abi != AEV_ARTIFACT_ABI_VERSION ||
        a->translator_abi != AEV_TRANSLATOR_ABI_VERSION) {
        if (error) *error = "AOT ABI/guest-state/artifact/translator mismatch";
        return false;
    }
    if (std::string(a->module_kind) != AEV_MODULE_KIND_PE64) {
        if (error) *error = std::string("AOT module kind is not PE: ") + a->module_kind;
        return false;
    }
    if (std::string(a->target) != AEV_AOT_TARGET_TRIPLE) {
        if (error) *error = std::string("AOT target mismatch: ") + a->target;
        return false;
    }
    const Module &m = *module;
    if (a->module_file_size != m.file_size ||
        a->preferred_base != m.preferred_base ||
        a->load_base != m.load_base ||
        a->image_size != m.size_of_image ||
        std::memcmp(a->image_sha256, m.image_sha256, 32) != 0 ||
        std::memcmp(a->import_binding_sha256, m.import_binding_sha256, 32) != 0 ||
        std::memcmp(a->host_binding_sha256, m.host_binding_sha256, 32) != 0 ||
        std::memcmp(a->entry_exports_sha256, m.entry_exports_sha256, 32) != 0 ||
        a->thunk_base != m.thunk_base ||
        a->import_count != m.unique_imports.size() ||
        a->relocs_stripped != (m.relocs_stripped ? 1 : 0)) {
        if (error) *error = "AOT module identity does not match loaded PE module";
        return false;
    }
    if (std::memcmp(a->input_sha256, m.file_sha256, 32) != 0 ||
        *a->input_hash != m.file_fnv || *a->entry_addr != m.entry_va) {
        if (error) *error = "AOT input/entry identity mismatch";
        return false;
    }
    uint8_t function_sha[32];
    aev::sha256(entry_export.data(), entry_export.size(), function_sha);
    if (std::memcmp(a->function_sha256, function_sha, 32) != 0) {
        if (error) *error = "AOT entry export hash mismatch";
        return false;
    }
    if (a->import_count > 256 || (a->import_count > 0 && !a->import_thunks)) {
        if (error) *error = "AOT import table is invalid";
        return false;
    }
    for (size_t i = 0; i < m.unique_imports.size(); ++i) {
        if (a->import_thunks[i] != m.unique_imports[i].thunk_va) {
            if (error) *error = "AOT import thunk table mismatch";
            return false;
        }
    }

    if (*a->all_count != all_blocks.size()) {
        if (error) *error = "AOT all-block count does not match program";
        return false;
    }
    for (size_t i = 0; i < all_blocks.size(); ++i) {
        if (a->all_blocks[i] != all_blocks[i]) {
            if (error) *error = "AOT all-block table does not match program";
            return false;
        }
    }
    if (*a->block_count > all_blocks.size()) {
        if (error) *error = "AOT selected count exceeds all blocks";
        return false;
    }
    std::set<uint64_t> selected;
    for (uint64_t i = 0; i < *a->block_count; ++i) {
        uint64_t rip = a->blocks[i].rip;
        if (!a->blocks[i].fn ||
            !std::binary_search(all_blocks.begin(), all_blocks.end(), rip) ||
            (i > 0 && a->blocks[i - 1].rip >= rip) ||
            !selected.insert(rip).second) {
            if (error) *error = "AOT selected block table is invalid";
            return false;
        }
        char symbol[64];
        std::snprintf(symbol, sizeof(symbol), "aev_blk_%llx",
                      (unsigned long long)rip);
        void *expected = dlsym(a->handle, symbol);
        if (!expected || reinterpret_cast<void *>(a->blocks[i].fn) != expected) {
            if (error) *error = "AOT selected block fn/symbol mismatch";
            return false;
        }
    }
    selected_blocks->assign(selected.begin(), selected.end());
    if (!aev::sha256_file(path, artifact_sha, error)) {
        if (error && error->empty()) *error = "cannot hash AOT artifact";
        return false;
    }
    return true;
}

bool peek_aot_kind(const std::string &path, std::string *kind,
                   std::string *error) {
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        if (error) *error = std::string("dlopen AOT failed: ") + dlerror();
        return false;
    }
    const char *value = static_cast<const char *>(dlsym(handle, "aev_module_kind"));
    bool ok = value != nullptr;
    if (ok && kind) *kind = value;
    if (!ok && error) *error = "AOT dylib missing aev_module_kind";
    dlclose(handle);
    return ok;
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

}  // namespace

struct SessionImpl {
    Module module;
    Program program;
    std::unique_ptr<JitRuntime> jit;
    AotInfo aot;
    std::vector<uint64_t> selected_blocks;
    std::vector<uint64_t> all_blocks;
    std::map<uint64_t, const Import *> imports_by_thunk;
    std::map<uint64_t, HostImpl> host_by_id;
    TraceWriter trace;
    AevTraceSink trace_sink{};
    std::string entry_export;
    uint8_t artifact_sha[32] = {};
    std::atomic<bool> in_call{false};
    std::thread::id owner;
    bool loaded = false;
    SessionCounts counts;
    SessionObserver observer;
    bool has_observer = false;
    uint64_t trace_call_base = 0;

    SessionImpl() : owner(std::this_thread::get_id()) {}

    ~SessionImpl() {
        trace.close();
        jit.reset();
        if (aot.handle) dlclose(aot.handle);
    }

    bool is_owner() const { return std::this_thread::get_id() == owner; }

    void cleanup() {
        trace.close();
        jit.reset();
        if (aot.handle) {
            dlclose(aot.handle);
            aot.handle = nullptr;
        }
        aot = AotInfo{};
        imports_by_thunk.clear();
        host_by_id.clear();
        selected_blocks.clear();
        all_blocks.clear();
        module = Module{};
        program = Program{};
        entry_export.clear();
        std::memset(artifact_sha, 0, sizeof(artifact_sha));
        counts = SessionCounts{};
        loaded = false;
        trace_call_base = 0;
        has_observer = false;
        observer = SessionObserver{};
    }

    static int64_t dispatch_thunk(void *opaque, GuestState *s, uint64_t rip);

    SessionStatus load(const SessionConfig &config, std::string *error);
    SessionStatus call(const std::string &export_name, const uint64_t args[4],
                       SessionCallResult *out, std::string *error);
    SessionStatus trace_open(const std::string &path,
                             const std::string &scenario_label,
                             std::string *error);
    SessionStatus trace_close(std::string *error);
    SessionStatus set_observer(const SessionObserver *obs, std::string *error);

    bool write_trace_header(const std::string &scenario_label, std::string *error);
};

int64_t SessionImpl::dispatch_thunk(void *opaque, GuestState *s, uint64_t rip) {
    SessionImpl *self = static_cast<SessionImpl *>(opaque);
    const Module &m = self->module;
    uint64_t thunk_end = m.thunk_base + m.unique_imports.size() * kThunkStride;
    if (rip >= m.thunk_base && rip < thunk_end) {
        auto it = self->imports_by_thunk.find(rip);
        if (it == self->imports_by_thunk.end()) {
            s->error_code = AEV_ERR_BAD_TARGET;
            s->error_pc = rip;
            self->trace.dispatch_miss(rip, (int)SessionStatus::UnknownImport);
            return -1;
        }
        if (s->pending_transfer != AEV_TRANSFER_CALL) {
            s->error_code = AEV_ERR_BAD_TARGET;
            s->error_pc = rip;
            self->trace.dispatch_miss(rip, (int)SessionStatus::Jit);
            return -1;
        }
        uint64_t ret = aev_load(s, s->rsp, 8, 0);
        if (s->error_code) {
            self->trace.dispatch_miss(rip, (int)SessionStatus::Memory);
            return -1;
        }
        s->rsp += 8;
        auto hit = self->host_by_id.find(it->second->host_import_id);
        if (hit == self->host_by_id.end()) {
            s->error_code = AEV_ERR_HOST;
            s->error_pc = rip;
            self->trace.dispatch_miss(rip, (int)SessionStatus::UnknownImport);
            return -1;
        }
        uint64_t out_rax = 0;
        int status = 1;
        try {
            status = hit->second.fn(s->rcx, s->rdx, s->r8, s->r9, &out_rax);
        } catch (...) {
            status = 1;
        }
        self->counts.host_import_calls++;
        self->trace.host_import(it->second->host_import_id,
                                it->second->thunk_index);
        if (self->has_observer && self->observer.on_host_import) {
            try {
                self->observer.on_host_import(self->observer.ctx,
                                              it->second->host_import_id,
                                              it->second->thunk_index);
            } catch (...) {
                /* Observers cannot affect execution or escape the C ABI. */
            }
        }
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
    int64_t rc = self->jit->dispatch(s, rip);
    if (rc != 0) {
        if (!s->error_code) s->error_code = AEV_ERR_JIT;
        self->trace.dispatch_miss(rip, map_guest_status(*s));
    }
    return rc;
}

SessionStatus SessionImpl::load(const SessionConfig &config, std::string *error) {
    if (loaded) {
        if (error) *error = "session is already loaded";
        return SessionStatus::InvalidArgument;
    }
    if (config.dll_path.empty()) {
        if (error) *error = "dll_path is required";
        return SessionStatus::InvalidArgument;
    }
    if (config.registry_path.empty()) {
        if (error) *error = "registry_path is required";
        return SessionStatus::InvalidArgument;
    }
    if (config.load_base == 0 && config.aot_path.empty()) {
        if (error) *error = "load base is required without an AOT artifact";
        return SessionStatus::InvalidArgument;
    }
    if (!config.aot_path.empty() && config.entry_export.empty()) {
        if (error) *error = "entry export is required with an AOT artifact";
        return SessionStatus::InvalidArgument;
    }

    std::string local_error;
    std::string *err = error ? error : &local_error;
    err->clear();

    ImportRegistry registry;
    if (!registry.load(config.registry_path, err)) {
        cleanup();
        return SessionStatus::Load;
    }

    uint64_t load_base = config.load_base;
    if (!config.aot_path.empty()) {
        std::string kind;
        if (!peek_aot_kind(config.aot_path, &kind, err)) {
            cleanup();
            return SessionStatus::AbiMismatch;
        }
        if (kind != AEV_MODULE_KIND_PE64) {
            if (err) *err = "AOT module kind is not PE: " + kind;
            cleanup();
            return SessionStatus::AbiMismatch;
        }
        uint64_t artifact_base = 0;
        if (!peek_aot_load_base(config.aot_path, &artifact_base, err)) {
            cleanup();
            return SessionStatus::AbiMismatch;
        }
        if (load_base == 0) {
            load_base = artifact_base;
        } else if (artifact_base != load_base) {
            if (err) *err = "artifact load_base does not match requested load_base";
            cleanup();
            return SessionStatus::AbiMismatch;
        }
    }

    LoadOptions load_options;
    load_options.load_base = load_base;
    if (!aev::pe::load_module(config.dll_path, load_options, registry, &module,
                              err)) {
        SessionStatus status = err->rfind("unknown import", 0) == 0
                                   ? SessionStatus::UnknownImport
                                   : SessionStatus::Load;
        cleanup();
        return status;
    }
    entry_export = config.entry_export;
    if (entry_export.empty()) {
        if (module.exports.empty()) {
            if (err) *err = "module has no exports";
            cleanup();
            return SessionStatus::Load;
        }
        entry_export = module.exports.front().name;
    }
    const Export *entry = module.find_export(entry_export);
    if (!entry) {
        if (err) *err = "entry export not found: " + entry_export;
        cleanup();
        return SessionStatus::Load;
    }
    module.primary_export = entry_export;
    module.entry_va = module.load_base + entry->rva;

    std::vector<std::string> roots;
    for (const auto &e : module.exports) roots.push_back(e.name);
    std::vector<aev::FunctionSpec> specs;
    if (!aev::pe::discover_functions(module, roots, &specs, err)) {
        cleanup();
        return SessionStatus::Load;
    }
    if (!aev::pe::build_program_for_module(module, specs, &program, err)) {
        cleanup();
        return SessionStatus::Load;
    }
    for (auto &fn : program.functions) {
        for (auto &b : fn.blocks) {
            b.jit_only = true;
            b.jit_reason = "no AOT artifact (JIT-only run)";
        }
    }

    all_blocks.clear();
    for (const auto &fn : program.functions)
        for (const auto &b : fn.blocks) all_blocks.push_back(b.address);
    std::sort(all_blocks.begin(), all_blocks.end());

    for (const auto &imp : module.unique_imports) {
        if (imp.host_import_id == 1) {
            HostImpl impl;
            impl.id = 1;
            impl.args = 2;
            impl.ctx = nullptr;
            void *ctx = impl.ctx;
            impl.fn = [ctx](uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t *out) {
                return host_update(ctx, a0, a1, a2, a3, out);
            };
            host_by_id[1] = impl;
        } else {
            if (err) {
                *err = "no host implementation for import id " +
                       std::to_string(imp.host_import_id);
            }
            cleanup();
            return SessionStatus::UnknownImport;
        }
        imports_by_thunk[imp.thunk_va] = &imp;
    }

    if (!config.aot_path.empty()) {
        if (!load_aot_file(&module, entry_export, all_blocks, &aot,
                           &selected_blocks, artifact_sha, config.aot_path,
                           err)) {
            cleanup();
            return SessionStatus::AbiMismatch;
        }
        if (module.load_base != aot.load_base) {
            if (err) *err = "artifact load_base does not match requested load_base";
            cleanup();
            return SessionStatus::AbiMismatch;
        }
        std::set<uint64_t> selected(selected_blocks.begin(),
                                    selected_blocks.end());
        for (auto &fn : program.functions) {
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
        std::memset(artifact_sha, 0, sizeof(artifact_sha));
    }

    jit = std::make_unique<aev::JitRuntime>(&program);
    if (!jit->init(err)) {
        cleanup();
        return SessionStatus::Jit;
    }

    trace.program = &program;
    trace.module = &module;
    trace.entry_export = entry_export;
    std::memcpy(trace.artifact_sha, artifact_sha, 32);
    trace.aot_selected = selected_blocks;
    trace.all_blocks = all_blocks;
    trace_sink.abi_version = AEV_TRACE_SINK_ABI_VERSION;
    trace_sink.on_block = trace_sink_block;
    trace_sink.ctx = &trace;
    jit->set_trace_hook(&trace, [](void *ctx, uint32_t source, uint64_t rip) {
        static_cast<TraceWriter *>(ctx)->write_block(source, rip);
    });

    if (!config.trace_path.empty()) {
        SessionStatus status = trace_open(config.trace_path, config.scenario, err);
        if (status != SessionStatus::Ok) {
            cleanup();
            return status;
        }
    }

    loaded = true;
    counts.load_count = 1;
    return SessionStatus::Ok;
}

bool SessionImpl::write_trace_header(const std::string &scenario_label,
                                     std::string *error) {
    if (!trace.stream) {
        if (error) *error = "cannot open trace file";
        return false;
    }
    uint8_t function_sha[32];
    aev::sha256(entry_export.data(), entry_export.size(), function_sha);
    std::ofstream &out = trace.stream;
    out << "{\"record\":\"header\",\"trace_abi\":" << AEV_TRACE_FILE_ABI_PE
        << ",\"scenario\":\""
        << json_escape(scenario_label.empty() ? std::string("A") : scenario_label)
        << "\",\"module_kind\":\"" << AEV_MODULE_KIND_PE64 << "\""
        << ",\"input_sha256\":\"" << aev::hex_bytes(module.file_sha256, 32)
        << "\",\"input_fnv\":\"" << hex(module.file_fnv) << "\""
        << ",\"module_file_size\":" << module.file_size
        << ",\"preferred_base\":\"" << hex(module.preferred_base) << "\""
        << ",\"load_base\":\"" << hex(module.load_base) << "\""
        << ",\"image_size\":" << module.size_of_image
        << ",\"image_sha256\":\"" << aev::hex_bytes(module.image_sha256, 32)
        << "\",\"import_binding_sha256\":\""
        << aev::hex_bytes(module.import_binding_sha256, 32)
        << "\",\"host_binding_sha256\":\""
        << aev::hex_bytes(module.host_binding_sha256, 32)
        << "\",\"entry_exports_sha256\":\""
        << aev::hex_bytes(module.entry_exports_sha256, 32)
        << "\",\"entry_exports\":[";
    for (size_t i = 0; i < module.exports.size(); ++i) {
        if (i) out << ",";
        out << "\"" << json_escape(module.exports[i].name) << "\"";
    }
    out << "],\"entry\":\"" << json_escape(entry_export) << "\""
        << ",\"function\":\"" << json_escape(entry_export) << "\""
        << ",\"entry_addr\":\"" << hex(module.entry_va) << "\""
        << ",\"thunk_base\":\"" << hex(module.thunk_base) << "\""
        << ",\"import_count\":" << module.unique_imports.size()
        << ",\"entry_function_sha256\":\""
        << aev::hex_bytes(function_sha, 32) << "\""
        << ",\"offset_kind\":\"rva\""
        << ",\"guest_abi\":2,\"aot_artifact_abi\":2,\"translator_abi\":2"
        << ",\"callback_abi\":3"
        << ",\"target\":\"" << AEV_AOT_TARGET_TRIPLE << "\""
        << ",\"translator_version\":\"aev-translate-v2\""
        << ",\"trace_tool_version\":\"aev-pe-trace-v1\""
        << ",\"aot_sha256\":\"" << aev::hex_bytes(artifact_sha, 32) << "\""
        << ",\"all_blocks\":[";
    for (size_t i = 0; i < all_blocks.size(); ++i) {
        if (i) out << ",";
        out << "\"" << hex(all_blocks[i]) << "\"";
    }
    out << "],\"aot_selected\":[";
    for (size_t i = 0; i < selected_blocks.size(); ++i) {
        if (i) out << ",";
        out << "\"" << hex(selected_blocks[i]) << "\"";
    }
    out << "]}\n";
    out.flush();
    if (!out) {
        if (error) *error = "cannot write trace header";
        return false;
    }
    return true;
}

SessionStatus SessionImpl::trace_open(const std::string &path,
                                      const std::string &scenario_label,
                                      std::string *error) {
    if (!is_owner()) {
        if (error) *error = "session owner thread mismatch";
        return SessionStatus::WrongThread;
    }
    if (in_call.load()) {
        if (error) *error = "trace_open is not allowed during a Guest call";
        return SessionStatus::Reentry;
    }
    if (path.empty()) {
        if (error) *error = "trace path is required";
        return SessionStatus::InvalidArgument;
    }
    if (trace.active) {
        if (error) *error = "trace is already open";
        return SessionStatus::InvalidArgument;
    }
    trace.stream.clear();
    trace.stream.open(path, std::ios::out | std::ios::trunc);
    if (!trace.stream) {
        if (error) *error = "cannot open trace file";
        return SessionStatus::Trace;
    }
    trace.active = true;
    trace.call_index = 0;
    trace.call_serial = 0;
    trace.records = 0;
    trace_call_base = 0;
    trace.program = &program;
    trace.module = &module;
    trace.entry_export = entry_export;
    std::memcpy(trace.artifact_sha, artifact_sha, 32);
    trace.aot_selected = selected_blocks;
    trace.all_blocks = all_blocks;
    if (!write_trace_header(scenario_label, error)) {
        trace.close();
        return SessionStatus::Trace;
    }
    counts.trace_open = 1;
    return SessionStatus::Ok;
}

SessionStatus SessionImpl::trace_close(std::string *error) {
    (void)error;
    if (!is_owner()) return SessionStatus::WrongThread;
    if (in_call.load()) return SessionStatus::Reentry;
    trace.close();
    counts.trace_open = 0;
    return SessionStatus::Ok;
}

SessionStatus SessionImpl::set_observer(const SessionObserver *obs,
                                        std::string *error) {
    if (!is_owner()) {
        if (error) *error = "session owner thread mismatch";
        return SessionStatus::WrongThread;
    }
    if (in_call.load()) {
        if (error) *error = "set_observer is not allowed during a Guest call";
        return SessionStatus::Reentry;
    }
    if (obs) {
        observer = *obs;
        has_observer = true;
    } else {
        observer = SessionObserver{};
        has_observer = false;
    }
    return SessionStatus::Ok;
}

SessionStatus SessionImpl::call(const std::string &export_name,
                                const uint64_t args[4],
                                SessionCallResult *out, std::string *error) {
    SessionCallResult result;
    if (out) *out = result;
    if (!is_owner()) {
        if (error) *error = "session owner thread mismatch";
        if (out) {
            out->status = (uint32_t)SessionStatus::WrongThread;
            if (args) std::memcpy(out->args, args, sizeof(out->args));
        }
        return SessionStatus::WrongThread;
    }
    if (args) std::memcpy(result.args, args, sizeof(result.args));
    if (out) *out = result;

    bool expected = false;
    if (!in_call.compare_exchange_strong(expected, true)) {
        if (error) *error = "reentrant session call rejected";
        if (out) out->status = (uint32_t)SessionStatus::Reentry;
        return SessionStatus::Reentry;
    }
    struct InCallGuard {
        std::atomic<bool> &flag;
        explicit InCallGuard(std::atomic<bool> &f) : flag(f) { flag = true; }
        ~InCallGuard() { flag = false; }
    } guard(in_call);

    const Export *e = module.find_export(export_name);
    if (!e) {
        if (error) *error = "export not found: " + export_name;
        if (out) out->status = (uint32_t)SessionStatus::InvalidArgument;
        return SessionStatus::InvalidArgument;
    }
    const aev::GuestFunction *fn = program.find_function_by_name(export_name);
    if (!fn) {
        if (error) *error = "export has no Program function: " + export_name;
        if (out) out->status = (uint32_t)SessionStatus::InvalidArgument;
        return SessionStatus::InvalidArgument;
    }

    GuestState state{};
    state.reserved1 = (uint64_t)(uintptr_t)module.regions.data();
    state.reserved2 = module.regions.size();
    state.reserved3 = module.load_base;
    state.rsp = module.entry_rsp;
    state.rip = module.load_base + e->rva;
    state.rcx = result.args[0];
    state.rdx = result.args[1];
    state.r8 = result.args[2];
    state.r9 = result.args[3];
    state.reserved0 = trace.active ? (uint64_t)(uintptr_t)&trace_sink : 0;

    bool entered = false;
    int status = (int)SessionStatus::Ok;
    uint64_t sentinel = AEV_RETURN_SENTINEL;
    if (!write_guest(&state, state.rsp, &sentinel, 8)) {
        status = map_guest_status(state);
    } else {
        uint8_t shadow[0x20] = {};
        if (!write_guest(&state, state.rsp + 8, shadow, sizeof(shadow))) {
            status = map_guest_status(state);
        } else {
            entered = true;
            counts.calls_total++;
            if (trace.active) {
                ++trace.call_serial;
                trace.call_index = trace_call_base + trace.call_serial;
            }
            AevRuntime rt;
            rt.jit_dispatch = &SessionImpl::dispatch_thunk;
            rt.ctx = this;
            int rc = 0;
            if (aot.run) {
                rc = aot.run(&state, &rt);
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
                    int64_t d = dispatch_thunk(this, &state, state.rip);
                    if (d != 0) {
                        rc = state.error_code ? (int)state.error_code
                                              : (int)AEV_ERR_JIT;
                        break;
                    }
                }
                if (rc == 0 && state.rip != AEV_RETURN_SENTINEL)
                    rc = (int)AEV_ERR_STEP_LIMIT;
            }
            if (rc == 0) {
                status = (state.rip == AEV_RETURN_SENTINEL &&
                          state.rsp == module.entry_rsp + 8 &&
                          state.error_code == 0)
                             ? (int)SessionStatus::Ok
                             : (int)SessionStatus::Internal;
            } else {
                status = state.error_code ? map_guest_status(state)
                                          : (int)SessionStatus::Internal;
            }
        }
    }

    counts.aot_blocks_total += state.aot_blocks;
    counts.jit_blocks_total += state.jit_blocks;
    counts.jit_compiles_total += state.jit_compiles;
    counts.jit_execs_total += state.jit_execs;

    if (entered) trace.call_end(state, status);

    result.status = (uint32_t)status;
    result.guest_error = (uint32_t)state.error_code;
    result.rax = state.rax;
    result.rax_i32 = (int32_t)(uint32_t)state.rax;
    result.error_pc = state.error_pc;
    result.mem_fault_kind = state.mem_fault_kind;
    result.mem_fault_addr = state.mem_fault_addr;
    result.mem_fault_size = state.mem_fault_size;
    result.mem_access = state.mem_access;
    result.steps = state.steps;
    result.aot_blocks = state.aot_blocks;
    result.jit_blocks = state.jit_blocks;
    result.jit_compiles = state.jit_compiles;
    result.jit_execs = state.jit_execs;
    result.call_index = entered ? counts.calls_total : 0;
    result.total_calls = counts.calls_total;
    result.total_aot_blocks = counts.aot_blocks_total;
    result.total_jit_blocks = counts.jit_blocks_total;
    result.total_jit_compiles = counts.jit_compiles_total;
    result.total_jit_execs = counts.jit_execs_total;
    result.guest_rip = state.rip;
    result.guest_rsp = state.rsp;

    if (entered && has_observer && observer.on_call_end) {
        try {
            observer.on_call_end(observer.ctx, &result);
        } catch (...) {
            /* Observers cannot affect execution or escape the C ABI. */
        }
    }

    if (out) *out = result;
    if (status != (int)SessionStatus::Ok && error) {
        *error = std::string("guest call failed: ") +
                 session_status_name((uint32_t)status);
    } else if (error) {
        error->clear();
    }
    return (SessionStatus)status;
}

const char *session_status_name(uint32_t status) {
    switch (status) {
        case 0: return "OK";
        case 2: return "INVALID_ARGUMENT";
        case 3: return "LOAD";
        case 4: return "ABI_MISMATCH";
        case 5: return "UNKNOWN_IMPORT";
        case 6: return "MEMORY";
        case 7: return "JIT";
        case 8: return "STEP_LIMIT";
        case 9: return "STALE_HANDLE";
        case 10: return "REENTRY";
        case 11: return "HOST";
        case 12: return "INTERNAL";
        case 13: return "WRONG_THREAD";
        case 14: return "TRACE";
        default: return "UNKNOWN";
    }
}

Session::Session() : impl_(std::make_unique<SessionImpl>()) {}
Session::~Session() = default;

SessionStatus Session::load(const SessionConfig &config, std::string *error) {
    return impl_->load(config, error);
}

SessionStatus Session::call(const std::string &export_name,
                            const uint64_t args[4], SessionCallResult *out,
                            std::string *error) {
    return impl_->call(export_name, args, out, error);
}

SessionStatus Session::trace_open(const std::string &path,
                                  const std::string &scenario,
                                  std::string *error) {
    return impl_->trace_open(path, scenario, error);
}

SessionStatus Session::trace_close(std::string *error) {
    return impl_->trace_close(error);
}

bool Session::trace_is_open() const { return impl_->trace.active; }

void Session::set_trace_call_base(uint64_t base) {
    impl_->trace_call_base = base;
}

SessionStatus Session::set_observer(const SessionObserver *observer,
                                    std::string *error) {
    return impl_->set_observer(observer, error);
}

bool Session::is_owner_thread() const { return impl_->is_owner(); }
bool Session::in_call() const { return impl_->in_call.load(); }

SessionStatus Session::check_destroy() const {
    if (!impl_->is_owner()) return SessionStatus::WrongThread;
    if (impl_->in_call.load()) return SessionStatus::Reentry;
    return SessionStatus::Ok;
}

const Module &Session::module() const { return impl_->module; }
const Program &Session::program() const { return impl_->program; }
const std::vector<uint64_t> &Session::all_blocks() const {
    return impl_->all_blocks;
}
const std::vector<uint64_t> &Session::selected_blocks() const {
    return impl_->selected_blocks;
}
const std::string &Session::entry_export() const { return impl_->entry_export; }
const uint8_t *Session::artifact_sha256() const { return impl_->artifact_sha; }
bool Session::aot_present() const { return impl_->aot.handle != nullptr; }
bool Session::loaded() const { return impl_->loaded; }
SessionCounts Session::counts() const {
    SessionCounts c = impl_->counts;
    c.trace_records = impl_->trace.records;
    c.trace_open = impl_->trace.active ? 1 : 0;
    return c;
}

SessionTable &SessionTable::instance() {
    static SessionTable table;
    return table;
}

uint64_t SessionTable::add(std::unique_ptr<Session> session) {
    if (!session) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i]) {
            if (generations_[i] == 0) generations_[i] = 1;
            slots_[i] = std::shared_ptr<Session>(std::move(session));
            return (uint64_t)(i + 1) | ((uint64_t)generations_[i] << 32);
        }
    }
    return 0;
}

std::shared_ptr<Session> SessionTable::get(uint64_t token) const {
    uint32_t slot_plus = (uint32_t)(token & 0xffffffffULL);
    uint32_t gen = (uint32_t)(token >> 32);
    if (slot_plus == 0 || slot_plus > slots_.size()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    size_t i = slot_plus - 1;
    if (!slots_[i] || generations_[i] != gen) return nullptr;
    return slots_[i];
}

SessionStatus SessionTable::destroy(uint64_t token, std::string *error) {
    uint32_t slot_plus = (uint32_t)(token & 0xffffffffULL);
    uint32_t gen = (uint32_t)(token >> 32);
    if (slot_plus == 0 || slot_plus > slots_.size()) {
        if (error) *error = "stale session handle";
        return SessionStatus::StaleHandle;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    size_t i = slot_plus - 1;
    if (!slots_[i] || generations_[i] != gen) {
        if (error) *error = "stale session handle";
        return SessionStatus::StaleHandle;
    }
    SessionStatus status = slots_[i]->check_destroy();
    if (status != SessionStatus::Ok) {
        if (error) {
            *error = status == SessionStatus::WrongThread
                         ? "session owner thread mismatch"
                         : "session destroy reentered during a Guest call";
        }
        return status;
    }
    slots_[i].reset();
    ++generations_[i];
    if (generations_[i] == 0) generations_[i] = 1;
    return SessionStatus::Ok;
}

}  // namespace aev::pe
