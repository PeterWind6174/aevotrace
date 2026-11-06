#ifndef AEV_BRIDGE_H
#define AEV_BRIDGE_H

/*
 * P0 callback bridge C ABI (trial v3).
 *
 * This is deliberately not the frozen P1 ABI.  It supports one single-level
 * Host->Guest->Host call on the process main thread, four integer/pointer
 * arguments, checked Guest memory, explicit HostImport stubs and explicit
 * failure statuses.  See docs/p0/CALLBACK-ABI-20261105.md.
 */

#include <stddef.h>
#include <stdint.h>

#include "aev_artifact.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AEV_CALLBACK_ABI_VERSION 3u
#define AEV_BRIDGE_MAX_ARGS 4u
#define AEV_GUEST_SENTINEL 0xFFFFFFFFFFFFFF00ULL
#define AEV_BRIDGE_IMPORT_BASE 0x140010000ULL
#define AEV_BRIDGE_IMPORT_STRIDE 0x10ULL
#define AEV_BRIDGE_IMPORT_LIMIT 0x140020000ULL
#define AEV_BRIDGE_STEP_LIMIT 1000000ULL

typedef struct AevBridge AevBridge;

typedef struct AevArtifactInfo {
    uint32_t struct_size;
    uint32_t abi_version;          /* AEV_ARTIFACT_INFO_ABI_VERSION */
    uint32_t aot_table_count;      /* aev_block_count, not per-call aot_blocks */
    uint32_t all_block_count;
    uint32_t artifact_abi_version;
    uint32_t translator_abi_version;
    uint64_t entry_addr;
    uint8_t input_sha256[32];
    uint8_t aot_sha256[32];
    char target_triple[48];
} AevArtifactInfo;

typedef enum AevBridgeStatus {
    AEV_BRIDGE_OK = 0,
    AEV_BRIDGE_ERR_INVALID_ARGUMENT = 1,
    AEV_BRIDGE_ERR_ABI_MISMATCH = 2,
    AEV_BRIDGE_ERR_LOAD = 3,
    AEV_BRIDGE_ERR_NO_BLOCK = 4,
    AEV_BRIDGE_ERR_UNKNOWN_IMPORT = 5,
    AEV_BRIDGE_ERR_MEMORY = 6,
    AEV_BRIDGE_ERR_JIT = 7,
    AEV_BRIDGE_ERR_STEP_LIMIT = 8,
    AEV_BRIDGE_ERR_REENTRY = 9,
    AEV_BRIDGE_ERR_WRONG_THREAD = 10,
    AEV_BRIDGE_ERR_HOST = 11,
    AEV_BRIDGE_ERR_INTERNAL = 12
} AevBridgeStatus;

typedef struct AevBridgeConfig {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *coff_path;
    const char *function_name;
    uint64_t entry_addr;
    const char *aot_dylib_path;
    uint64_t guest_mem_size;
    uint64_t guest_stack_size;
    uint32_t trace;
    uint32_t dump_jit;
    uint32_t trace_aot;
    uint32_t require_main_thread;
    uint32_t reserved0;
} AevBridgeConfig;

typedef struct AevCallResult {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t status;
    uint32_t guest_error;
    uint64_t guest_rax;
    uint64_t guest_rip;
    uint64_t guest_rsp;
    uint64_t error_pc;
    uint64_t steps;
    uint64_t aot_blocks;          /* per-call delta */
    uint64_t jit_blocks;          /* per-call delta */
    uint64_t jit_compiles;        /* per-call delta */
    uint64_t jit_execs;           /* per-call delta */
    uint64_t host_import_id;
    uint64_t call_index;          /* 1-based; 0 means no Guest entry */
    uint64_t guest_mem_base;
    uint64_t guest_mem_size;
    uint64_t total_calls;
    uint64_t total_aot_blocks;
    uint64_t total_jit_blocks;
    uint64_t total_jit_compiles;
    uint64_t total_jit_execs;
} AevCallResult;

/*
 * a0..a3 are the Guest call-site RCX/RDX/R8/R9 values.  The helper must
 * return AEV_BRIDGE_OK and set *out_rax; any other status stops the call
 * without writing Guest RAX.  Host callbacks must not throw; C++ exceptions
 * are caught as a last resort and mapped to AEV_BRIDGE_ERR_HOST.  The
 * fixture helper uses a0..a2 and ignores a3 (raw R9, the GuestCtx pointer).
 */
typedef AevBridgeStatus (*AevHostImportFn)(void *host_ctx, AevBridge *bridge,
                                           uint64_t a0, uint64_t a1,
                                           uint64_t a2, uint64_t a3,
                                           uint64_t *out_rax);

int aev_bridge_abi_info(uint32_t *abi_version, uint32_t *guest_state_size,
                        uint32_t *call_result_size, uint32_t *config_size);
AevBridgeStatus aev_bridge_create(const AevBridgeConfig *cfg,
                                  AevBridge **out_bridge,
                                  char *err, size_t err_cap);
AevBridgeStatus aev_bridge_destroy(AevBridge *b);
int aev_bridge_guest_info(const AevBridge *b, uint64_t *base, uint64_t *size,
                          uint64_t *stack_base, uint64_t *stack_top);
AevBridgeStatus aev_bridge_artifact_info(const AevBridge *b, AevArtifactInfo *info,
                                         char *err, size_t err_cap);
/* Actual selected AOT table introspection (not the manifest).  Returns 1/0
 * for contains; negative for invalid arguments. */
int aev_bridge_selected_block_contains(const AevBridge *b, uint64_t rip);
uint32_t aev_bridge_selected_block_count(const AevBridge *b);
int aev_bridge_selected_block_rip(const AevBridge *b, uint32_t index, uint64_t *rip);
/* Test-only: exposes the transient GuestState.reserved0 trace sink pointer. */
uint64_t aev_bridge_debug_reserved0(const AevBridge *b);
int aev_bridge_register_import(AevBridge *b, uint64_t guest_addr, uint64_t import_id,
                               AevHostImportFn fn, void *host_ctx,
                               char *err, size_t err_cap);
int aev_bridge_guest_write(AevBridge *b, uint64_t guest_addr, const void *src,
                           uint64_t len, char *err, size_t err_cap);
int aev_bridge_guest_read(AevBridge *b, uint64_t guest_addr, void *dst,
                          uint64_t len, char *err, size_t err_cap);
int aev_bridge_call(AevBridge *b, uint64_t entry_addr,
                    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                    AevCallResult *result, char *err, size_t err_cap);

/* Test-only fault injection; the GUI path must not call this.
 * mode=1: force a JIT failure when Guest reaches rip.
 * mode=2: after a normal sentinel return, corrupt RSP to exercise INTERNAL.
 * mode=0 clears the injection. */
int aev_bridge_set_test_fault(AevBridge *b, uint32_t mode, uint64_t rip,
                              char *err, size_t err_cap);

AevBridgeStatus aev_bridge_trace_open(AevBridge *b, const char *path,
                                      const char *scenario,
                                      char *err, size_t err_cap);
AevBridgeStatus aev_bridge_trace_close(AevBridge *b);

#ifdef __cplusplus
}
#endif

#endif /* AEV_BRIDGE_H */
