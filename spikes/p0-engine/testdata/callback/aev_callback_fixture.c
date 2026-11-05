typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short u16;
typedef struct GuestCtx {
    u64 (*update)(u64 hash, u64 len, u64 op);
    u64 *counter;
} GuestCtx;

#pragma code_seg(".aevtext")
__declspec(noinline)
u32 aev_guest_callback(unsigned int op, const u16 *text, u64 len, GuestCtx *ctx) {
    u32 h = 2166136261u;
    for (u64 i = 0; i < len; ++i) {
        h ^= (u32)text[i];
        h *= 16777619u;
    }
    u64 r = ctx->update((u64)h, len, (u64)op);
    (*ctx->counter)++;
    return h ^ (u32)r;
}
#pragma code_seg()
