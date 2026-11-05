#include <stdio.h>
#include <stdint.h>

typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short u16;
typedef struct GuestCtx {
    u64 (*update)(u64 hash, u64 len, u64 op);
    u64 *counter;
} GuestCtx;

u32 aev_guest_callback(unsigned int op, const u16 *text, u64 len, GuestCtx *ctx);

static u64 host_update(u64 hash, u64 len, u64 op) {
    printf("HELPER hash=%llu len=%llu op=%llu\n",
           (unsigned long long)hash, (unsigned long long)len,
           (unsigned long long)op);
    return op;
}

static int run_case(const char *name, unsigned int op, const u16 *text, u64 len,
                    GuestCtx *ctx) {
    u32 r = aev_guest_callback(op, text, len, ctx);
    printf("CASE %s op=%u len=%llu result=%u counter=%llu\n", name, op,
           (unsigned long long)len, r, (unsigned long long)*ctx->counter);
    return 0;
}

int main(void) {
    static const u16 alpha[] = { 0x0041, 0x0065, 0x0076, 0x006f, 0x03b2, 0x4e2d };
    static const u16 beta[] = { 0x006d, 0x0061, 0x0063, 0x004f, 0x0053, 0x002d, 0x0034, 0x0037 };
    static const u16 hidden[] = { 0x72ec, 0x7acb, 0x002d, 0x03a9, 0x002d, 0x0039, 0x0033 };
    GuestCtx ctx;
    u64 counter = 0;
    ctx.update = host_update;
    ctx.counter = &counter;
    run_case("alpha", 7, alpha, 6, &ctx);
    run_case("beta", 9, beta, 8, &ctx);
    run_case("hidden", 0x5a5a, hidden, 7, &ctx);
    return (counter == 3) ? 0 : 1;
}
