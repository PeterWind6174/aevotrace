#include "aev_guest.h"

#include <stdio.h>

extern const AevBlock aev_blocks[];
extern const uint64_t aev_block_count;

/* ABI/identity symbols checked by the callback bridge before it executes a
 * loaded dylib.  These are intentionally plain exported data symbols. */
const uint64_t aev_aot_abi_version = AEV_GUEST_ABI_VERSION;
const uint64_t aev_guest_state_size = sizeof(GuestState);

static const AevBlock *find_block(uint64_t rip) {
    for (uint64_t i = 0; i < aev_block_count; ++i) {
        if (aev_blocks[i].rip == rip)
            return &aev_blocks[i];
    }
    return 0;
}

int aev_aot_run(GuestState *s, const AevRuntime *rt) {
    const uint64_t step_limit = 1000000;

    for (uint64_t steps = 0; steps < step_limit; ++steps) {
        if (s->error_code) {
            fprintf(stderr, "AOT stopped with error=%llu rip=0x%llx\n",
                    (unsigned long long)s->error_code,
                    (unsigned long long)s->rip);
            return (int)s->error_code;
        }
        if (s->rip == AEV_RETURN_SENTINEL)
            return 0;

        s->steps = steps + 1;
        const AevBlock *block = find_block(s->rip);
        if (block) {
            block->fn(s);
            continue;
        }

        if (!rt || !rt->jit_dispatch) {
            fprintf(stderr, "AOT/JIT dispatch failed: no AOT block and no JIT callback for rip=0x%llx\n",
                    (unsigned long long)s->rip);
            s->error_code = AEV_ERR_NO_BLOCK;
            return AEV_ERR_NO_BLOCK;
        }

        int64_t rc = rt->jit_dispatch(rt->ctx, s, s->rip);
        if (rc != 0) {
            fprintf(stderr, "JIT dispatch failed for rip=0x%llx (rc=%lld)\n",
                    (unsigned long long)s->rip, (long long)rc);
            if (!s->error_code)
                s->error_code = AEV_ERR_JIT;
            return (int)s->error_code;
        }
    }

    fprintf(stderr, "AOT step limit exceeded at rip=0x%llx\n",
            (unsigned long long)s->rip);
    s->error_code = AEV_ERR_STEP_LIMIT;
    return AEV_ERR_STEP_LIMIT;
}
