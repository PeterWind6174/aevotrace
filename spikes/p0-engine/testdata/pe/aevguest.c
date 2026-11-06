/* P0-PE-DLL-01 fixture: no CRT, no DllMain, one IAT import, global state and
 * a DIR64-relocated pointer.  Built as x64 PE32+ DLL with MSVC /NOENTRY. */
typedef unsigned int u32;
typedef unsigned long long u64;

__declspec(dllimport) int AevUpdate(int a, int b);

typedef struct State {
    int seed;
    int table[8];
    int *cursor;
    u64 counter;
} State;

static State g_state = {
    0x1234,
    {3, 1, 4, 1, 5, 9, 2, 6},
    &g_state.table[2],
    0
};
static int g_calls = 0;

#define AEV_FRAME() volatile int aev_frame_pad = 0; (void)aev_frame_pad

__declspec(noinline) int AevDllReset(int seed) {
    AEV_FRAME();
    int i;
    g_state.seed = seed;
    for (i = 0; i < 8; ++i) {
        g_state.table[i] =
            (int)(((u32)seed + ((u32)i << 3) - (u32)i) ^ 0x5A5A5A5Au);
    }
    g_state.cursor = &g_state.table[2];
    g_state.counter = 0;
    g_calls = 0;
    return seed;
}

__declspec(noinline) int AevDllCompute(int a, int b, int n) {
    AEV_FRAME();
    int acc = (int)((u32)g_state.seed ^ (u32)a);
    int i;
    for (i = 0; i < n; ++i) {
        int v = g_state.table[i & 7];
        if (v & 1) {
            acc = (int)((u32)acc + (u32)v + (u32)v + (u32)v);
        } else {
            acc = (int)((u32)acc - ((u32)v + (u32)i));
        }
        acc = (int)(((u32)acc << 1) | ((u32)acc >> 31));
    }
    if (((u32)a & 4u) != 0u) {
        acc = (int)((u32)acc + (u32)g_state.table[5]);
    } else {
        acc = (int)((u32)acc - (u32)g_state.table[6]);
    }
    {
        int h = AevUpdate(a + g_calls, b);
        g_state.counter += (u64)(u32)acc;
        g_state.counter += (u64)(u32)h;
        g_calls++;
        acc = (int)((u32)acc ^ (u32)(*g_state.cursor));
    }
    return acc;
}

__declspec(noinline) int AevDllState(void) {
    AEV_FRAME();
    return (int)((u32)g_state.counter ^ (u32)g_calls);
}

__declspec(noinline) int AevDllCursorValue(void) {
    AEV_FRAME();
    return *g_state.cursor;
}

__declspec(noinline) unsigned long long AevDllCursorAddress(void) {
    AEV_FRAME();
    return (unsigned long long)(void *)g_state.cursor;
}

__declspec(noinline) int AevDllCalls(void) {
    AEV_FRAME();
    return g_calls;
}

__declspec(noinline) int AevDllReadAt(unsigned long long addr) {
    AEV_FRAME();
    volatile unsigned int *p = (volatile unsigned int *)(unsigned long long)addr;
    return (int)*p;
}

__declspec(noinline) int AevDllWriteAt(unsigned long long addr, int value) {
    AEV_FRAME();
    volatile unsigned int *p = (volatile unsigned int *)(unsigned long long)addr;
    *p = (unsigned int)value;
    return value;
}

__declspec(noinline) int AevDllCallPtr(unsigned long long addr) {
    AEV_FRAME();
    int (*fn)(void) = (int (*)(void))(unsigned long long)addr;
    return fn();
}
