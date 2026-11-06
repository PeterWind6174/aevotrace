#include <stdio.h>
#include <stdint.h>

typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct GuestCtx {
    u64 (*update)(u64 result, u64 x, u64 op);
    u64 *counter;
} GuestCtx;

u32 aev_trace_guest(unsigned int op, unsigned int x, GuestCtx *ctx);

static u64 host_update(u64 result, u64 x, u64 op) {
    printf("HELPER result=%llu x=%llu op=%llu\n",
           (unsigned long long)result, (unsigned long long)x,
           (unsigned long long)op);
    return op;
}

int main(void) {
    GuestCtx ctx;
    u64 counter = 0;
    ctx.update = host_update;
    ctx.counter = &counter;
    u32 a = aev_trace_guest(1, 7, &ctx);
    printf("CASE A op=1 x=7 result=%u counter=%llu\n", a,
           (unsigned long long)counter);
    u32 b = aev_trace_guest(0xA5A5u, 9, &ctx);
    printf("CASE B op=%u x=9 result=%u counter=%llu\n", 0xA5A5u, b,
           (unsigned long long)counter);
    return counter == 2 ? 0 : 1;
}
