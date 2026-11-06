typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct GuestCtx {
    u64 (*update)(u64 result, u64 x, u64 op);
    u64 *counter;
} GuestCtx;

/*
 * P0-TRACE-AOT-01 fixed x64 fixture.
 * A path (op != 0xA5A5) is a simple add/xor.  B path (op == 0xA5A5) is a
 * distinct branch body containing IMUL, not executed by A.  Helper returns op.
 * The function lives in its own .aevtext section so block identity/all-block
 * tables are not contaminated by neighbouring .text.
 */
#pragma code_seg(".aevtext")
__declspec(noinline)
u32 aev_trace_guest(unsigned int op, unsigned int x, GuestCtx *ctx) {
    u32 result;
    if (op == 0xA5A5u) {
        result = (x * 17u) ^ 0x1234u;
        result += 0x200u;
    } else {
        result = (x + 0x55u) ^ 0x0F0Fu;
    }
    u64 h = ctx->update((u64)result, (u64)x, (u64)op);
    (*ctx->counter)++;
    return result ^ (u32)h;
}
#pragma code_seg()
