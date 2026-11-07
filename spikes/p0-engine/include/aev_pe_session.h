#ifndef AEV_PE_SESSION_H
#define AEV_PE_SESSION_H

/*
 * P0-PE-GUI-01 shared PE execution session C ABI (contract v1).
 *
 * This is deliberately not the frozen P1 ABI.  One session owns one bounded
 * PE32+ AMD64 DLL instance, its checked Guest memory, shared Program,
 * AOT/JIT runtime, declared HostImport implementation and optional trace.
 * The same implementation is used by aev-pe-runner (CLI) and the compat /
 * native GUI harness; no second PE loader or dispatcher is permitted.
 *
 * Handles are generation-checked 64-bit tokens, not raw pointers.  The owner
 * thread is the thread that created the session (the AppKit main thread in
 * the GUI path).  Calls from another thread, reentry from an observer or
 * HostImport, and destroy while a call is active are rejected explicitly.
 * See docs/p0/PE-SESSION-CONTRACT-20261107.md.
 */

#include <stddef.h>
#include <stdint.h>

#include "aev_artifact.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AEV_PE_SESSION_ABI_VERSION 1u
#define AEV_PE_SESSION_MAX_ARGS 4u
#define AEV_PE_SESSION_MAX_SLOTS 8u
/* Includes the terminating NUL: at most 511 name bytes are representable in
 * this bounded P0 ABI; longer PE names are truncated in query outputs. */
#define AEV_PE_SESSION_MAX_NAME 512u

typedef uint64_t AevPeSession; /* 0 is invalid */

typedef enum AevPeSessionStatus {
    AEV_PE_SESSION_OK = 0,
    AEV_PE_SESSION_ERR_INVALID_ARGUMENT = 2,
    AEV_PE_SESSION_ERR_LOAD = 3,
    AEV_PE_SESSION_ERR_ABI_MISMATCH = 4,
    AEV_PE_SESSION_ERR_UNKNOWN_IMPORT = 5,
    AEV_PE_SESSION_ERR_MEMORY = 6,
    AEV_PE_SESSION_ERR_JIT = 7,
    AEV_PE_SESSION_ERR_STEP_LIMIT = 8,
    AEV_PE_SESSION_ERR_STALE_HANDLE = 9,
    AEV_PE_SESSION_ERR_REENTRY = 10,
    AEV_PE_SESSION_ERR_HOST = 11,
    AEV_PE_SESSION_ERR_INTERNAL = 12,
    AEV_PE_SESSION_ERR_WRONG_THREAD = 13,
    AEV_PE_SESSION_ERR_TRACE = 14
} AevPeSessionStatus;

typedef struct AevPeSessionConfig {
    uint32_t struct_size;       /* must equal sizeof(AevPeSessionConfig) */
    uint32_t abi_version;       /* must equal AEV_PE_SESSION_ABI_VERSION */
    const char *dll_path;       /* required */
    const char *registry_path;  /* required */
    const char *aot_path;       /* NULL/empty: JIT-only */
    /* With aot_path, entry_export must be non-NULL/non-empty and is the
     * artifact-bound entry.  Without aot_path, NULL/empty selects the first
     * named PE export in PE export-table order. */
    const char *entry_export;
    uint64_t load_base;         /* 0 requires aot_path */
    const char *trace_path;     /* NULL/empty: trace disabled at create */
    const char *scenario;       /* optional trace scenario label */
    uint32_t flags;             /* must be 0 */
    uint32_t reserved0;
    uint64_t reserved1;
} AevPeSessionConfig;

/*
 * Output structures (AbiInfo excepted): the caller sets struct_size to the
 * exact ctypes/C sizeof and abi_version to AEV_PE_SESSION_ABI_VERSION; the
 * library checks the size and overwrites abi_version.  A mismatch returns
 * ABI_MISMATCH.  AevPeSessionAbiInfo accepts struct_size 0 or its exact size.
 */
typedef struct AevPeSessionAbiInfo {
    uint32_t struct_size;
    uint32_t abi_version;       /* AEV_PE_SESSION_ABI_VERSION */
    uint32_t config_size;
    uint32_t result_size;
    uint32_t info_size;
    uint32_t counts_size;
    uint32_t export_info_size;
    uint32_t block_info_size;
    uint64_t guest_abi_version;
    uint64_t guest_state_size;
    uint64_t artifact_abi_version;
    uint64_t translator_abi_version;
    uint64_t callback_abi_version;
    uint64_t max_args;
} AevPeSessionAbiInfo;

typedef struct AevPeCallResult {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t status;            /* AevPeSessionStatus */
    uint32_t guest_error;       /* GuestState.error_code (AEV_ERR_*) */
    uint64_t args[AEV_PE_SESSION_MAX_ARGS]; /* raw RCX/RDX/R8/R9 bits */
    uint64_t rax;               /* raw RAX bits */
    int32_t rax_i32;            /* low 32 bits interpreted as signed */
    uint32_t reserved0;
    uint64_t error_pc;
    uint64_t mem_fault_kind;
    uint64_t mem_fault_addr;
    uint64_t mem_fault_size;
    uint64_t mem_access;
    uint64_t steps;
    uint64_t aot_blocks;        /* per-call delta */
    uint64_t jit_blocks;        /* per-call delta */
    uint64_t jit_compiles;      /* per-call delta */
    uint64_t jit_execs;         /* per-call delta */
    uint64_t call_index;        /* 1-based; 0 if Guest was not entered */
    uint64_t total_calls;
    uint64_t total_aot_blocks;
    uint64_t total_jit_blocks;
    uint64_t total_jit_compiles;
    uint64_t total_jit_execs;
    uint64_t guest_rip;
    uint64_t guest_rsp;
} AevPeCallResult;

typedef struct AevPeModuleInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t guest_abi_version;
    uint32_t artifact_abi_version;
    uint32_t translator_abi_version;
    uint32_t callback_abi_version;
    uint64_t guest_state_size;
    uint64_t file_size;
    uint64_t preferred_base;
    uint64_t load_base;
    uint64_t image_size;
    uint64_t thunk_base;
    uint64_t import_count;
    uint64_t relocs_stripped;
    uint64_t entry_va;
    uint64_t stack_base;
    uint64_t stack_top;
    uint64_t entry_rsp;
    uint64_t region_count;
    uint64_t all_block_count;
    uint64_t selected_count;
    uint64_t aot_present;
    uint64_t load_count;
    uint8_t file_sha256[32];
    uint8_t image_sha256[32];
    uint8_t import_binding_sha256[32];
    uint8_t host_binding_sha256[32];
    uint8_t entry_exports_sha256[32];
    uint8_t aot_sha256[32];
    char module_kind[24];
    char entry_export[AEV_PE_SESSION_MAX_NAME];
    char target_triple[48];
} AevPeModuleInfo;

typedef struct AevPeSessionCounts {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t load_count;
    uint64_t calls_total;
    uint64_t host_import_calls;
    uint64_t aot_blocks_total;
    uint64_t jit_blocks_total;
    uint64_t jit_compiles_total;
    uint64_t jit_execs_total;
    uint64_t trace_records;
    uint64_t trace_open;
} AevPeSessionCounts;

typedef struct AevPeExportInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t rva;
    uint32_t ordinal;
    uint64_t va;
    char name[AEV_PE_SESSION_MAX_NAME];
} AevPeExportInfo;

typedef struct AevPeBlockInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t rip;
    uint32_t rva;
    uint32_t size;
    uint8_t sha256[32];
    uint32_t aot_selected;
    uint32_t reserved;
} AevPeBlockInfo;

/*
 * Diagnostic observer, called on the owner thread while a Guest call is
 * active.  set_observer copies this structure; the caller's pointer may be
 * temporary, but the two function pointers and ctx must remain valid until
 * set_observer(NULL) or destroy.  on_call_end's result pointer is valid only
 * during that callback.  in_call remains set through both callbacks, so
 * call/destroy/trace_open/trace_close/set_observer from a callback returns
 * REENTRY.  C++ exceptions are caught; callbacks implemented in other
 * languages must not propagate their own exceptions across this C ABI.
 */
typedef struct AevPeSessionObserver {
    uint32_t struct_size;       /* must equal sizeof(AevPeSessionObserver) */
    uint32_t abi_version;       /* must equal AEV_PE_SESSION_ABI_VERSION */
    void (*on_call_end)(void *ctx, const AevPeCallResult *result);
    void (*on_host_import)(void *ctx, uint64_t host_import_id,
                           uint32_t thunk_index);
} AevPeSessionObserver;

/* No session required.  out->struct_size may be 0 (library fills its own
 * size) or the caller's exact size. */
AevPeSessionStatus aev_pe_session_abi_info(AevPeSessionAbiInfo *out);
const char *aev_pe_session_status_name(uint32_t status);

/* Create, load, bind and trace-open in that order.  On failure *out_handle is
 * 0 and all partially loaded Host backing/JIT/trace resources are released.
 * The owner thread is the calling thread. */
AevPeSessionStatus aev_pe_session_create(const AevPeSessionConfig *cfg,
                                         AevPeSession *out_handle,
                                         char *err, size_t err_cap);

/* Stale token -> STALE_HANDLE; double destroy -> STALE_HANDLE.  A non-owner
 * thread -> WRONG_THREAD; destroy from the owner while a call is active ->
 * REENTRY.  Process-wide state is otherwise unaffected. */
AevPeSessionStatus aev_pe_session_destroy(AevPeSession handle);

AevPeSessionStatus aev_pe_session_info(AevPeSession handle, AevPeModuleInfo *out,
                                       char *err, size_t err_cap);
AevPeSessionStatus aev_pe_session_counts(AevPeSession handle,
                                         AevPeSessionCounts *out,
                                         char *err, size_t err_cap);

AevPeSessionStatus aev_pe_session_export_count(AevPeSession handle,
                                               uint32_t *out_count,
                                               char *err, size_t err_cap);
AevPeSessionStatus aev_pe_session_export_info(AevPeSession handle, uint32_t index,
                                              AevPeExportInfo *out,
                                              char *err, size_t err_cap);
AevPeSessionStatus aev_pe_session_find_export(AevPeSession handle,
                                              const char *name,
                                              AevPeExportInfo *out,
                                              char *err, size_t err_cap);

AevPeSessionStatus aev_pe_session_selected_count(AevPeSession handle,
                                                 uint32_t *out_count,
                                                 char *err, size_t err_cap);
AevPeSessionStatus aev_pe_session_selected_rip(AevPeSession handle, uint32_t index,
                                               uint64_t *out_rip,
                                               char *err, size_t err_cap);
AevPeSessionStatus aev_pe_session_all_block_count(AevPeSession handle,
                                                  uint32_t *out_count,
                                                  char *err, size_t err_cap);
AevPeSessionStatus aev_pe_session_all_block_rip(AevPeSession handle, uint32_t index,
                                                uint64_t *out_rip,
                                                char *err, size_t err_cap);
AevPeSessionStatus aev_pe_session_block_info(AevPeSession handle, uint64_t rip,
                                             AevPeBlockInfo *out,
                                             char *err, size_t err_cap);

/* args are raw 64-bit register bits.  The GUI convention is to zero-extend
 * the low 32 bits of signed business int32 inputs; the result is interpreted
 * as int32 through rax_i32, not by comparing the full RAX bit pattern. */
AevPeSessionStatus aev_pe_session_call(AevPeSession handle, const char *export_name,
                                       const uint64_t args[AEV_PE_SESSION_MAX_ARGS],
                                       AevPeCallResult *out,
                                       char *err, size_t err_cap);

/* Trace open truncates the file, writes a PE trace ABI 4 header and resets
 * the call index; opening an already-open trace -> INVALID_ARGUMENT.  Close
 * flushes and is idempotent (OK when already closed).  Both are owner-only
 * and rejected with REENTRY during a call. */
AevPeSessionStatus aev_pe_session_trace_open(AevPeSession handle, const char *path,
                                             const char *scenario,
                                             char *err, size_t err_cap);
AevPeSessionStatus aev_pe_session_trace_close(AevPeSession handle,
                                              char *err, size_t err_cap);

/* Observer registration is owner-only.  Passing NULL clears it. */
AevPeSessionStatus aev_pe_session_set_observer(AevPeSession handle,
                                               const AevPeSessionObserver *observer,
                                               void *ctx,
                                               char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* AEV_PE_SESSION_H */
