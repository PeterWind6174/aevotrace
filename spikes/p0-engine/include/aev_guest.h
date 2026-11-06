#ifndef AEV_GUEST_H
#define AEV_GUEST_H

/*
 * P0-ENGINE-01 spike ABI.  This is intentionally not a frozen product ABI.
 *
 * The guest x86-64 register file is kept in one contiguous, 8-byte aligned
 * structure so the LLVM-generated ARM64 blocks and the C runtime agree on
 * offsets.  All fields are uint64_t to avoid host-struct padding surprises.
 */

#include <stdint.h>

#include "aev_artifact.h"

#define AEV_FIELD(name) uint64_t name;

typedef struct GuestState {
#include "aev_guest_fields.def"
} GuestState;

#undef AEV_FIELD

/* Transient trace sink stored in GuestState.reserved0 only while a bridge
 * trace is active.  It is bridge-owned, cleared on every call exit, and is
 * never persisted in trace/artifact data. */
typedef struct AevTraceSink {
    uint32_t abi_version;
    uint32_t reserved;
    void (*on_block)(void *ctx, uint32_t source, uint64_t rip);
    void *ctx;
} AevTraceSink;

#define AEV_RETURN_SENTINEL 0xFFFFFFFFFFFFFF00ULL
#define AEV_GUEST_ABI_VERSION 1ULL
#define AEV_GUEST_STATE_SIZE (32ULL * 8ULL)

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
    AEV_ERR_INTERNAL = 6
};

typedef struct AevRuntime {
    /*
     * Called by the AOT dispatcher when a guest RIP has no AOT block.
     * ctx is opaque to the AOT runtime; the runner owns it.
     * Returns 0 after the JIT-compiled block has run and updated s->rip.
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
void aev_note_aot(GuestState *s, uint64_t rip);

#ifdef __cplusplus
}
#endif

#endif /* AEV_GUEST_H */
