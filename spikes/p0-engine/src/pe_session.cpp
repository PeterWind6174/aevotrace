#include "aev_pe_session.h"

#include "aev_bridge.h"
#include "aev_guest.h"
#include "pe_session_core.hpp"
#include "program_identity.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

using aev::pe::Session;
using aev::pe::SessionCallResult;
using aev::pe::SessionConfig;
using aev::pe::SessionCounts;
using aev::pe::SessionObserver;
using aev::pe::SessionStatus;

void write_error(char *err, size_t cap, const std::string &message) {
    if (!err || cap == 0) return;
    size_t n = message.size();
    if (n > cap - 1) n = cap - 1;
    if (n) std::memcpy(err, message.data(), n);
    err[n] = '\0';
}

void clear_error(char *err, size_t cap) {
    if (err && cap) err[0] = '\0';
}

AevPeSessionStatus fail(AevPeSessionStatus status, char *err, size_t cap,
                        const std::string &message) {
    write_error(err, cap, message);
    return status;
}

AevPeSessionStatus success(char *err, size_t cap) {
    clear_error(err, cap);
    return AEV_PE_SESSION_OK;
}

template <typename Fn>
AevPeSessionStatus guard_capi(char *err, size_t cap, Fn &&fn) {
    try {
        return fn();
    } catch (const std::exception &e) {
        return fail(AEV_PE_SESSION_ERR_INTERNAL, err, cap,
                    std::string("internal error: ") + e.what());
    } catch (...) {
        return fail(AEV_PE_SESSION_ERR_INTERNAL, err, cap, "internal error");
    }
}

bool check_out_struct(uint32_t size, size_t expected, char *err, size_t cap,
                      const char *what) {
    if (size != expected) {
        write_error(err, cap,
                    std::string(what) + " struct_size mismatch (caller=" +
                        std::to_string(size) + " library=" +
                        std::to_string(expected) + ")");
        return false;
    }
    return true;
}

std::shared_ptr<Session> resolve(AevPeSession handle, char *err, size_t cap,
                                 AevPeSessionStatus *status) {
    auto session = aev::pe::SessionTable::instance().get(handle);
    if (!session) {
        *status = fail(AEV_PE_SESSION_ERR_STALE_HANDLE, err, cap,
                       "stale session handle");
        return nullptr;
    }
    if (!session->is_owner_thread()) {
        *status = fail(AEV_PE_SESSION_ERR_WRONG_THREAD, err, cap,
                       "session owner thread mismatch");
        return nullptr;
    }
    *status = AEV_PE_SESSION_OK;
    return session;
}

void copy_name(char *dst, size_t cap, const std::string &src) {
    if (!dst || cap == 0) return;
    size_t n = src.size();
    if (n > cap - 1) n = cap - 1;
    if (n) std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void fill_abi_versions(uint32_t *guest_abi, uint32_t *artifact_abi,
                       uint32_t *translator_abi, uint32_t *callback_abi,
                       uint64_t *guest_state_size) {
    if (guest_abi) *guest_abi = (uint32_t)AEV_GUEST_ABI_VERSION;
    if (artifact_abi) *artifact_abi = AEV_ARTIFACT_ABI_VERSION;
    if (translator_abi) *translator_abi = AEV_TRANSLATOR_ABI_VERSION;
    if (callback_abi) *callback_abi = AEV_CALLBACK_ABI_VERSION;
    if (guest_state_size) *guest_state_size = (uint64_t)sizeof(GuestState);
}

void fill_result(AevPeCallResult *out, const SessionCallResult &r) {
    if (!out) return;
    out->struct_size = (uint32_t)sizeof(AevPeCallResult);
    out->abi_version = AEV_PE_SESSION_ABI_VERSION;
    out->status = r.status;
    out->guest_error = r.guest_error;
    std::memcpy(out->args, r.args, sizeof(out->args));
    out->rax = r.rax;
    out->rax_i32 = r.rax_i32;
    out->reserved0 = 0;
    out->error_pc = r.error_pc;
    out->mem_fault_kind = r.mem_fault_kind;
    out->mem_fault_addr = r.mem_fault_addr;
    out->mem_fault_size = r.mem_fault_size;
    out->mem_access = r.mem_access;
    out->steps = r.steps;
    out->aot_blocks = r.aot_blocks;
    out->jit_blocks = r.jit_blocks;
    out->jit_compiles = r.jit_compiles;
    out->jit_execs = r.jit_execs;
    out->call_index = r.call_index;
    out->total_calls = r.total_calls;
    out->total_aot_blocks = r.total_aot_blocks;
    out->total_jit_blocks = r.total_jit_blocks;
    out->total_jit_compiles = r.total_jit_compiles;
    out->total_jit_execs = r.total_jit_execs;
    out->guest_rip = r.guest_rip;
    out->guest_rsp = r.guest_rsp;
}

void fill_empty_result(AevPeCallResult *out, const uint64_t args[4],
                       AevPeSessionStatus status) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(AevPeCallResult);
    out->abi_version = AEV_PE_SESSION_ABI_VERSION;
    out->status = (uint32_t)status;
    if (args) std::memcpy(out->args, args, sizeof(out->args));
}

/* C ABI observer callbacks receive AevPeCallResult, while the core passes its
 * internal SessionCallResult.  The trampolines convert and keep the C function
 * pointers out of the core execution path.  The binding is process-wide but
 * owner-thread-only, so no session API is called from the trampoline. */
struct CObserverState {
    void *ctx = nullptr;
    void (*on_call_end)(void *, const AevPeCallResult *) = nullptr;
    void (*on_host_import)(void *, uint64_t, uint32_t) = nullptr;

    static void call_end_trampoline(void *opaque,
                                    const SessionCallResult *result) {
        CObserverState *self = static_cast<CObserverState *>(opaque);
        if (!self || !self->on_call_end || !result) return;
        AevPeCallResult converted;
        fill_result(&converted, *result);
        try {
            self->on_call_end(self->ctx, &converted);
        } catch (...) {
            /* Observer exceptions never escape or change the call result. */
        }
    }

    static void host_import_trampoline(void *opaque, uint64_t host_import_id,
                                       uint32_t thunk_index) {
        CObserverState *self = static_cast<CObserverState *>(opaque);
        if (!self || !self->on_host_import) return;
        try {
            self->on_host_import(self->ctx, host_import_id, thunk_index);
        } catch (...) {
        }
    }
};

std::mutex g_observer_mutex;
std::map<AevPeSession, std::shared_ptr<CObserverState>> g_observers;

void erase_observer(AevPeSession handle) {
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    g_observers.erase(handle);
}

}  // namespace

extern "C" {

AevPeSessionStatus aev_pe_session_abi_info(AevPeSessionAbiInfo *out) {
    return guard_capi(nullptr, 0, [&]() -> AevPeSessionStatus {
        if (!out) return AEV_PE_SESSION_ERR_INVALID_ARGUMENT;
        if (out->struct_size != 0 && out->struct_size != sizeof(AevPeSessionAbiInfo))
            return AEV_PE_SESSION_ERR_ABI_MISMATCH;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(AevPeSessionAbiInfo);
        out->abi_version = AEV_PE_SESSION_ABI_VERSION;
        out->config_size = (uint32_t)sizeof(AevPeSessionConfig);
        out->result_size = (uint32_t)sizeof(AevPeCallResult);
        out->info_size = (uint32_t)sizeof(AevPeModuleInfo);
        out->counts_size = (uint32_t)sizeof(AevPeSessionCounts);
        out->export_info_size = (uint32_t)sizeof(AevPeExportInfo);
        out->block_info_size = (uint32_t)sizeof(AevPeBlockInfo);
        out->guest_abi_version = AEV_GUEST_ABI_VERSION;
        out->guest_state_size = (uint64_t)sizeof(GuestState);
        out->artifact_abi_version = AEV_ARTIFACT_ABI_VERSION;
        out->translator_abi_version = AEV_TRANSLATOR_ABI_VERSION;
        out->callback_abi_version = AEV_CALLBACK_ABI_VERSION;
        out->max_args = AEV_PE_SESSION_MAX_ARGS;
        return AEV_PE_SESSION_OK;
    });
}

const char *aev_pe_session_status_name(uint32_t status) {
    return aev::pe::session_status_name(status);
}

AevPeSessionStatus aev_pe_session_create(const AevPeSessionConfig *cfg,
                                         AevPeSession *out_handle, char *err,
                                         size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out_handle) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err,
                                     err_cap, "out_handle is required");
        *out_handle = 0;
        if (!cfg) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err,
                              err_cap, "cfg is required");
        if (cfg->struct_size != sizeof(AevPeSessionConfig))
            return fail(AEV_PE_SESSION_ERR_ABI_MISMATCH, err, err_cap,
                        "config struct_size mismatch");
        if (cfg->abi_version != AEV_PE_SESSION_ABI_VERSION)
            return fail(AEV_PE_SESSION_ERR_ABI_MISMATCH, err, err_cap,
                        "config abi_version mismatch");
        if (cfg->flags != 0 || cfg->reserved0 != 0 || cfg->reserved1 != 0)
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "config flags/reserved fields must be zero");
        if (!cfg->dll_path || !*cfg->dll_path)
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "dll_path is required");
        if (!cfg->registry_path || !*cfg->registry_path)
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "registry_path is required");

        SessionConfig config;
        config.dll_path = cfg->dll_path;
        config.registry_path = cfg->registry_path;
        if (cfg->aot_path) config.aot_path = cfg->aot_path;
        if (cfg->entry_export) config.entry_export = cfg->entry_export;
        config.load_base = cfg->load_base;
        if (cfg->trace_path) config.trace_path = cfg->trace_path;
        if (cfg->scenario) config.scenario = cfg->scenario;

        auto session = std::make_unique<Session>();
        std::string error;
        SessionStatus status = session->load(config, &error);
        if (status != SessionStatus::Ok) {
            write_error(err, err_cap,
                        error.empty() ? aev::pe::session_status_name((uint32_t)status)
                                      : error);
            return (AevPeSessionStatus)status;
        }
        uint64_t handle =
            aev::pe::SessionTable::instance().add(std::move(session));
        if (handle == 0) {
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "session table full");
        }
        *out_handle = handle;
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_destroy(AevPeSession handle) {
    return guard_capi(nullptr, 0, [&]() -> AevPeSessionStatus {
        std::string error;
        SessionStatus status =
            aev::pe::SessionTable::instance().destroy(handle, &error);
        if (status == SessionStatus::Ok) erase_observer(handle);
        return (AevPeSessionStatus)status;
    });
}

AevPeSessionStatus aev_pe_session_info(AevPeSession handle, AevPeModuleInfo *out,
                                       char *err, size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                              "out is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        if (!check_out_struct(out->struct_size, sizeof(AevPeModuleInfo), err,
                              err_cap, "module info"))
            return AEV_PE_SESSION_ERR_ABI_MISMATCH;
        const aev::pe::Module &m = session->module();
        SessionCounts counts = session->counts();
        std::memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(AevPeModuleInfo);
        out->abi_version = AEV_PE_SESSION_ABI_VERSION;
        fill_abi_versions(&out->guest_abi_version, &out->artifact_abi_version,
                          &out->translator_abi_version,
                          &out->callback_abi_version, &out->guest_state_size);
        out->file_size = m.file_size;
        out->preferred_base = m.preferred_base;
        out->load_base = m.load_base;
        out->image_size = m.size_of_image;
        out->thunk_base = m.thunk_base;
        out->import_count = m.unique_imports.size();
        out->relocs_stripped = m.relocs_stripped ? 1 : 0;
        out->entry_va = m.entry_va;
        out->stack_base = m.stack_base;
        out->stack_top = m.stack_top;
        out->entry_rsp = m.entry_rsp;
        out->region_count = m.regions.size();
        out->all_block_count = session->all_blocks().size();
        out->selected_count = session->selected_blocks().size();
        out->aot_present = session->aot_present() ? 1 : 0;
        out->load_count = counts.load_count;
        std::memcpy(out->file_sha256, m.file_sha256, 32);
        std::memcpy(out->image_sha256, m.image_sha256, 32);
        std::memcpy(out->import_binding_sha256, m.import_binding_sha256, 32);
        std::memcpy(out->host_binding_sha256, m.host_binding_sha256, 32);
        std::memcpy(out->entry_exports_sha256, m.entry_exports_sha256, 32);
        std::memcpy(out->aot_sha256, session->artifact_sha256(), 32);
        copy_name(out->module_kind, sizeof(out->module_kind),
                  AEV_MODULE_KIND_PE64);
        copy_name(out->entry_export, sizeof(out->entry_export),
                  session->entry_export());
        copy_name(out->target_triple, sizeof(out->target_triple),
                  AEV_AOT_TARGET_TRIPLE);
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_counts(AevPeSession handle,
                                         AevPeSessionCounts *out, char *err,
                                         size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                              "out is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        if (!check_out_struct(out->struct_size, sizeof(AevPeSessionCounts), err,
                              err_cap, "counts"))
            return AEV_PE_SESSION_ERR_ABI_MISMATCH;
        SessionCounts c = session->counts();
        std::memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(AevPeSessionCounts);
        out->abi_version = AEV_PE_SESSION_ABI_VERSION;
        out->load_count = c.load_count;
        out->calls_total = c.calls_total;
        out->host_import_calls = c.host_import_calls;
        out->aot_blocks_total = c.aot_blocks_total;
        out->jit_blocks_total = c.jit_blocks_total;
        out->jit_compiles_total = c.jit_compiles_total;
        out->jit_execs_total = c.jit_execs_total;
        out->trace_records = c.trace_records;
        out->trace_open = c.trace_open;
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_export_count(AevPeSession handle,
                                               uint32_t *out_count, char *err,
                                               size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out_count) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err,
                                    err_cap, "out_count is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        *out_count = (uint32_t)session->module().exports.size();
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_export_info(AevPeSession handle, uint32_t index,
                                              AevPeExportInfo *out, char *err,
                                              size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                              "out is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        if (!check_out_struct(out->struct_size, sizeof(AevPeExportInfo), err,
                              err_cap, "export info"))
            return AEV_PE_SESSION_ERR_ABI_MISMATCH;
        const auto &exports = session->module().exports;
        if (index >= exports.size())
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "export index out of range");
        const aev::pe::Export &e = exports[index];
        std::memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(AevPeExportInfo);
        out->abi_version = AEV_PE_SESSION_ABI_VERSION;
        out->rva = e.rva;
        out->ordinal = e.ordinal;
        out->va = session->module().load_base + e.rva;
        copy_name(out->name, sizeof(out->name), e.name);
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_find_export(AevPeSession handle,
                                              const char *name,
                                              AevPeExportInfo *out, char *err,
                                              size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!name || !*name) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT,
                                         err, err_cap, "name is required");
        if (!out) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                              "out is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        if (!check_out_struct(out->struct_size, sizeof(AevPeExportInfo), err,
                              err_cap, "export info"))
            return AEV_PE_SESSION_ERR_ABI_MISMATCH;
        const aev::pe::Export *e = session->module().find_export(name);
        if (!e)
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        std::string("export not found: ") + name);
        std::memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(AevPeExportInfo);
        out->abi_version = AEV_PE_SESSION_ABI_VERSION;
        out->rva = e->rva;
        out->ordinal = e->ordinal;
        out->va = session->module().load_base + e->rva;
        copy_name(out->name, sizeof(out->name), e->name);
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_selected_count(AevPeSession handle,
                                                 uint32_t *out_count, char *err,
                                                 size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out_count) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err,
                                    err_cap, "out_count is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        *out_count = (uint32_t)session->selected_blocks().size();
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_selected_rip(AevPeSession handle, uint32_t index,
                                               uint64_t *out_rip, char *err,
                                               size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out_rip) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err,
                                  err_cap, "out_rip is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        const auto &selected = session->selected_blocks();
        if (index >= selected.size())
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "selected index out of range");
        *out_rip = selected[index];
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_all_block_count(AevPeSession handle,
                                                  uint32_t *out_count, char *err,
                                                  size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out_count) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err,
                                    err_cap, "out_count is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        *out_count = (uint32_t)session->all_blocks().size();
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_all_block_rip(AevPeSession handle,
                                                uint32_t index,
                                                uint64_t *out_rip, char *err,
                                                size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out_rip) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err,
                                  err_cap, "out_rip is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        const auto &blocks = session->all_blocks();
        if (index >= blocks.size())
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "all-block index out of range");
        *out_rip = blocks[index];
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_block_info(AevPeSession handle, uint64_t rip,
                                             AevPeBlockInfo *out, char *err,
                                             size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!out) return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                              "out is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        if (!check_out_struct(out->struct_size, sizeof(AevPeBlockInfo), err,
                              err_cap, "block info"))
            return AEV_PE_SESSION_ERR_ABI_MISMATCH;
        aev::BlockIdentity id;
        std::string error;
        if (!aev::block_identity(session->program(), rip, &id, &error))
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        error);
        const auto &selected = session->selected_blocks();
        std::memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(AevPeBlockInfo);
        out->abi_version = AEV_PE_SESSION_ABI_VERSION;
        out->rip = rip;
        out->rva = id.section_offset;
        out->size = id.size;
        std::memcpy(out->sha256, id.sha256, 32);
        out->aot_selected =
            std::binary_search(selected.begin(), selected.end(), rip) ? 1 : 0;
        out->reserved = 0;
        return success(err, err_cap);
    });
}

AevPeSessionStatus aev_pe_session_call(AevPeSession handle,
                                       const char *export_name,
                                       const uint64_t args[AEV_PE_SESSION_MAX_ARGS],
                                       AevPeCallResult *out, char *err,
                                       size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!export_name || !*export_name)
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "export_name is required");
        if (!args)
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "args is required");
        if (!out)
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "out is required");
        if (!check_out_struct(out->struct_size, sizeof(AevPeCallResult), err,
                              err_cap, "call result"))
            return AEV_PE_SESSION_ERR_ABI_MISMATCH;
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) {
            fill_empty_result(out, args, status);
            return status;
        }
        SessionCallResult result;
        std::string error;
        SessionStatus call_status =
            session->call(export_name, args, &result, &error);
        fill_result(out, result);
        if (call_status == SessionStatus::Ok) {
            clear_error(err, err_cap);
        } else {
            write_error(err, err_cap, error.empty()
                                         ? aev::pe::session_status_name(
                                               (uint32_t)call_status)
                                         : error);
        }
        return (AevPeSessionStatus)call_status;
    });
}

AevPeSessionStatus aev_pe_session_trace_open(AevPeSession handle,
                                             const char *path,
                                             const char *scenario, char *err,
                                             size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        if (!path || !*path)
            return fail(AEV_PE_SESSION_ERR_INVALID_ARGUMENT, err, err_cap,
                        "trace path is required");
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        std::string error;
        SessionStatus trace_status =
            session->trace_open(path, scenario ? scenario : "", &error);
        if (trace_status == SessionStatus::Ok) return success(err, err_cap);
        write_error(err, err_cap, error.empty()
                                     ? aev::pe::session_status_name(
                                           (uint32_t)trace_status)
                                     : error);
        return (AevPeSessionStatus)trace_status;
    });
}

AevPeSessionStatus aev_pe_session_trace_close(AevPeSession handle, char *err,
                                              size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        std::string error;
        SessionStatus trace_status = session->trace_close(&error);
        if (trace_status == SessionStatus::Ok) return success(err, err_cap);
        write_error(err, err_cap, error.empty()
                                     ? aev::pe::session_status_name(
                                           (uint32_t)trace_status)
                                     : error);
        return (AevPeSessionStatus)trace_status;
    });
}

AevPeSessionStatus aev_pe_session_set_observer(
    AevPeSession handle, const AevPeSessionObserver *observer, void *ctx,
    char *err, size_t err_cap) {
    return guard_capi(err, err_cap, [&]() -> AevPeSessionStatus {
        AevPeSessionStatus status = AEV_PE_SESSION_OK;
        auto session = resolve(handle, err, err_cap, &status);
        if (!session) return status;
        if (observer) {
            if (observer->struct_size != sizeof(AevPeSessionObserver))
                return fail(AEV_PE_SESSION_ERR_ABI_MISMATCH, err, err_cap,
                            "observer struct_size mismatch");
            if (observer->abi_version != AEV_PE_SESSION_ABI_VERSION)
                return fail(AEV_PE_SESSION_ERR_ABI_MISMATCH, err, err_cap,
                            "observer abi_version mismatch");
            auto state = std::make_shared<CObserverState>();
            state->ctx = ctx;
            state->on_call_end = observer->on_call_end;
            state->on_host_import = observer->on_host_import;
            SessionObserver obs;
            obs.ctx = state.get();
            obs.on_call_end = &CObserverState::call_end_trampoline;
            obs.on_host_import = &CObserverState::host_import_trampoline;
            std::shared_ptr<CObserverState> previous;
            {
                std::lock_guard<std::mutex> lock(g_observer_mutex);
                auto it = g_observers.find(handle);
                if (it != g_observers.end()) previous = it->second;
                g_observers[handle] = state;
            }
            std::string error;
            SessionStatus obs_status = session->set_observer(&obs, &error);
            if (obs_status != SessionStatus::Ok) {
                std::lock_guard<std::mutex> lock(g_observer_mutex);
                if (previous) {
                    g_observers[handle] = previous;
                } else {
                    g_observers.erase(handle);
                }
                write_error(err, err_cap, error);
                return (AevPeSessionStatus)obs_status;
            }
        } else {
            std::string error;
            SessionStatus obs_status = session->set_observer(nullptr, &error);
            if (obs_status != SessionStatus::Ok) {
                write_error(err, err_cap, error);
                return (AevPeSessionStatus)obs_status;
            }
            erase_observer(handle);
        }
        return success(err, err_cap);
    });
}

}  // extern "C"
