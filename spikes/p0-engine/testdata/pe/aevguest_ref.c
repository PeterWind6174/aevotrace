/* Windows reference runner: loads the same fixed aevguest.dll through the
 * system loader and calls the same exports as the Mac converter.  This file is
 * not conversion input and is never compiled on macOS. */
#include <windows.h>
#include <stdio.h>

typedef int (*ResetFn)(int);
typedef int (*ComputeFn)(int, int, int);
typedef int (*IntFn)(void);
typedef unsigned int (*UIntFn)(void);
typedef unsigned long long (*AddrFn)(void);

static HMODULE load_checked(const char *name) {
    HMODULE h = LoadLibraryA(name);
    if (!h) {
        printf("load_error=%s code=%lu\n", name, (unsigned long)GetLastError());
        return NULL;
    }
    return h;
}

int main(void) {
    HMODULE helper = load_checked("aevhost.dll");
    if (!helper) return 2;
    HMODULE guest = load_checked("aevguest.dll");
    if (!guest) return 3;

    ResetFn reset = (ResetFn)(void *)GetProcAddress(guest, "AevDllReset");
    ComputeFn compute = (ComputeFn)(void *)GetProcAddress(guest, "AevDllCompute");
    IntFn state = (IntFn)(void *)GetProcAddress(guest, "AevDllState");
    IntFn cursor_value = (IntFn)(void *)GetProcAddress(guest, "AevDllCursorValue");
    AddrFn cursor_addr = (AddrFn)(void *)GetProcAddress(guest, "AevDllCursorAddress");
    IntFn calls = (IntFn)(void *)GetProcAddress(guest, "AevDllCalls");
    UIntFn helper_calls = (UIntFn)(void *)GetProcAddress(helper, "AevHelperCalls");
    if (!reset || !compute || !state || !cursor_value || !cursor_addr ||
        !calls || !helper_calls) {
        printf("missing_export\n");
        return 4;
    }

    reset(0x1234);
    printf("reset seed=%d cursor_value=%d cursor_addr=0x%llx state=%d calls=%d helper=%u\n",
           0x1234, cursor_value(), cursor_addr(), state(), calls(), helper_calls());

    struct Case {
        const char *name;
        int a, b, n;
    } cases[] = {
        {"prep", 11, 4, 8},
        {"hidden", -9, 2, 17},
        {"empty", 100, 101, 0},
        {"wrap", 2147483647, 1, 3},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reset(0x1234);
        int result = compute(cases[i].a, cases[i].b, cases[i].n);
        printf("case=%s a=%d b=%d n=%d result=%d state=%d cursor_value=%d "
               "cursor_addr=0x%llx calls=%d helper=%u\n",
               cases[i].name, cases[i].a, cases[i].b, cases[i].n, result,
               state(), cursor_value(), cursor_addr(), calls(), helper_calls());
    }
    return 0;
}
