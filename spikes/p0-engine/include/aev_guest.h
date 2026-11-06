#ifndef AEV_GUEST_H
#define AEV_GUEST_H

/*
 * P0-PE-DLL-01 Guest ABI v2 (contract v3).  Not a frozen product ABI.
 *
 * The guest x86-64 register file is kept in one contiguous, 8-byte aligned
 * structure so LLVM-generated ARM64 blocks and the C runtime agree on
 * offsets.  All fields are uint64_t; new fields are appended at the end.
 */

#include <stdint.h>

#include "aev_artifact.h"

#define AEV_FIELD(name) uint64_t name;

typedef struct GuestState {
#include "aev_guest_fields.def"
} GuestState;

#undef AEV_FIELD

/* Transient trace sink stored in GuestState.reserved0 only while a bridge /
 * PE runner trace is active.  Bridge-owned; cleared on every call exit. */
typedef struct AevTraceSink {
    uint32_t abi_version;
    uint32_t reserved;
    void (*on_block)(void *ctx, uint32_t source, uint64_t rip);
    void *ctx;
} AevTraceSink;

/* Runtime-only checked Guest memory region.  host_base must never be
 * persisted in artifacts, traces or Guest-visible memory. */
typedef struct AevMemRegion {
    uint64_t guest_base;
    uint64_t size;
    uint64_t host_base;
    uint32_t prot;
    uint32_t reserved;
} AevMemRegion;

#define AEV_MEM_PROT_READ 1u
#define AEV_MEM_PROT_WRITE 2u
#define AEV_MEM_PROT_EXEC 4u

#define AEV_MEM_FAULT_NONE 0ULL
#define AEV_MEM_FAULT_UNMAPPED 1ULL
#define AEV_MEM_FAULT_READ 2ULL
#define AEV_MEM_FAULT_WRITE 3ULL
#define AEV_MEM_FAULT_EXEC 4ULL
#define AEV_MEM_FAULT_RANGE 5ULL
#define AEV_MEM_FAULT_SIZE 6ULL

#define AEV_MEM_ACCESS_NONE 0ULL
#define AEV_MEM_ACCESS_READ 1ULL
#define AEV_MEM_ACCESS_WRITE 2ULL
#define AEV_MEM_ACCESS_EXEC 3ULL

#define AEV_TRANSFER_NONE 0ULL
#define AEV_TRANSFER_CALL 1ULL
#define AEV_TRANSFER_JMP 2ULL
#define AEV_TRANSFER_RET 3ULL
#define AEV_TRANSFER_JCC 4ULL

#define AEV_RETURN_SENTINEL 0xFFFFFFFFFFFFFF00ULL

/* x86-64 RFLAGS bits used by the spike. */
#define AEV_CF (1ULL << 0)
#define AEV_PF (1ULL << 2)
#define AEV_AF (1ULL << 4)
#define AEV_ZF (1ULL << 6)
#define AEV_SF (1ULL << 7)
#define AEV_OF (1ULL << 11)

/* Explicit error codes.  A non-zero value always stops the AOT dispatcher. */
enum {
    AEV_ERR_NONE = 0,
    AEV_ERR_MEMORY = 1,
    AEV_ERR_NO_BLOCK = 2,
    AEV_ERR_JIT = 3,
    AEV_ERR_STEP_LIMIT = 4,
    AEV_ERR_BAD_TARGET = 5,
    AEV_ERR_INTERNAL = 6,
    AEV_ERR_HOST = 7
};

typedef struct AevRuntime {
    /*
     * Called by the AOT dispatcher when a guest RIP has no AOT block.
     * ctx is opaque to the AOT runtime; the runner owns it.
     * Returns 0 after the JIT-compiled block / import thunk has run and
     * updated s->rip.  Import thunks are recognized by the callback before
     * JIT compilation; no Host pointer is passed through Guest state.
     */
    int64_t (*jit_dispatch)(void *ctx, GuestState *s, uint64_t rip);
    void *ctx;
} AevRuntime;

typedef struct AevBlock {
    uint64_t rip;
    void (*fn)(GuestState *s);
} AevBlock;

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime helpers called from generated code. */
uint64_t aev_load(GuestState *s, uint64_t addr, uint32_t size, uint32_t sign_extend);
void aev_store(GuestState *s, uint64_t addr, uint64_t value, uint32_t size);
void aev_flags_add(GuestState *s, uint32_t width, uint64_t a, uint64_t b, uint64_t r);
void aev_flags_sub(GuestState *s, uint32_t width, uint64_t a, uint64_t b, uint64_t r);
void aev_flags_logic(GuestState *s, uint32_t width, uint64_t r);
void aev_flags_inc(GuestState *s, uint32_t width, uint64_t a, uint64_t r);
void aev_flags_dec(GuestState *s, uint32_t width, uint64_t a, uint64_t r);
void aev_flags_neg(GuestState *s, uint32_t width, uint64_t a, uint64_t r);
void aev_flags_imul(GuestState *s, uint32_t width, uint64_t a, uint64_t b, uint64_t r);
void aev_flags_shift(GuestState *s, uint32_t width, uint64_t value, uint64_t count,
                     uint64_t result, uint32_t kind);
void aev_note_aot(GuestState *s, uint64_t rip);

/* Checked instruction-fetch permission.  Returns 1 when [rip,rip+len) is
 * executable, 0 otherwise (sets error_code/mem_fault_*).  In legacy COFF
 * identity mode (no region table) this returns 1; PE modules must use the
 * region table. */
int aev_guest_require_exec(GuestState *s, uint64_t rip, uint64_t len);

#ifdef __cplusplus
}
#endif

#endif /* AEV_GUEST_H */
