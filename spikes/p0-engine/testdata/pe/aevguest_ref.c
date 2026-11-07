/* Windows reference runner: loads the same fixed aevguest.dll through the
 * system loader and calls the same exports as the Mac converter.  This file is
 * not conversion input and is never compiled on macOS.
 *
 * Modes:
 *   aevguest_ref.exe           legacy fixed-case reference -> reference_stdout.txt
 *   aevguest_ref.exe stateful  GUI-round continuous sequence
 *                              -> reference_stateful_stdout.txt
 *
 * The stateful sequence mirrors the P0-PE-GUI-01 GUI round: one Reset, four
 * Compute inputs (including one outside the preparation set), then 100
 * consecutive Compute(5, 7, 3) calls with no reset in between.  Every step is
 * one line of step/result/state/cursor_value/cursor_addr/calls/helper fields;
 * a final line reports the ending state/calls/cursor/helper values.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef int (*ResetFn)(int);
typedef int (*ComputeFn)(int, int, int);
typedef int (*IntFn)(void);
typedef unsigned int (*UIntFn)(void);
typedef unsigned long long (*AddrFn)(void);

typedef struct Exports {
    ResetFn reset;
    ComputeFn compute;
    IntFn state;
    IntFn cursor_value;
    AddrFn cursor_addr;
    IntFn calls;
    UIntFn helper_calls;
} Exports;

static HMODULE load_checked(const char *name) {
    HMODULE h = LoadLibraryA(name);
    if (!h) {
        printf("load_error=%s code=%lu\n", name, (unsigned long)GetLastError());
        return NULL;
    }
    return h;
}

static int load_exports(Exports *e) {
    HMODULE helper = load_checked("aevhost.dll");
    HMODULE guest;
    if (!helper) return 2;
    guest = load_checked("aevguest.dll");
    if (!guest) return 3;

    memset(e, 0, sizeof(*e));
    e->reset = (ResetFn)(void *)GetProcAddress(guest, "AevDllReset");
    e->compute = (ComputeFn)(void *)GetProcAddress(guest, "AevDllCompute");
    e->state = (IntFn)(void *)GetProcAddress(guest, "AevDllState");
    e->cursor_value = (IntFn)(void *)GetProcAddress(guest, "AevDllCursorValue");
    e->cursor_addr = (AddrFn)(void *)GetProcAddress(guest, "AevDllCursorAddress");
    e->calls = (IntFn)(void *)GetProcAddress(guest, "AevDllCalls");
    e->helper_calls = (UIntFn)(void *)GetProcAddress(helper, "AevHelperCalls");
    if (!e->reset || !e->compute || !e->state || !e->cursor_value ||
        !e->cursor_addr || !e->calls || !e->helper_calls) {
        printf("missing_export\n");
        return 4;
    }
    return 0;
}

/* Read-only observation of the DLL's exported state, never a host shadow. */
static void print_state_fields(const Exports *e) {
    int state = e->state();
    int cursor_value = e->cursor_value();
    unsigned long long cursor_addr = e->cursor_addr();
    int calls = e->calls();
    unsigned int helper = e->helper_calls();
    printf("state=%d cursor_value=%d cursor_addr=0x%llx calls=%d helper=%u",
           state, cursor_value, cursor_addr, calls, helper);
}

static int run_legacy(const Exports *e) {
    e->reset(0x1234);
    printf("reset seed=%d cursor_value=%d cursor_addr=0x%llx state=%d calls=%d helper=%u\n",
           0x1234, e->cursor_value(), e->cursor_addr(), e->state(), e->calls(), e->helper_calls());

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
        e->reset(0x1234);
        int result = e->compute(cases[i].a, cases[i].b, cases[i].n);
        printf("case=%s a=%d b=%d n=%d result=%d state=%d cursor_value=%d "
               "cursor_addr=0x%llx calls=%d helper=%u\n",
               cases[i].name, cases[i].a, cases[i].b, cases[i].n, result,
               e->state(), e->cursor_value(), e->cursor_addr(), e->calls(), e->helper_calls());
    }
    return 0;
}

static int run_stateful(const Exports *e) {
    struct Step {
        int a, b, n;
    } steps[] = {
        {11, 4, 8},
        {-9, 2, 17},
        {100, 101, 0},
        {(int)0x13572468, 0x2468, 5},  /* independent input outside prep set */
    };
    int i;
    int result;

    result = e->reset(0x1234);
    printf("step=reset seed=%d result=%d ", 0x1234, result);
    print_state_fields(e);
    printf("\n");

    for (i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); ++i) {
        result = e->compute(steps[i].a, steps[i].b, steps[i].n);
        printf("step=compute index=%d a=%d b=%d n=%d result=%d ",
               i + 1, steps[i].a, steps[i].b, steps[i].n, result);
        print_state_fields(e);
        printf("\n");
    }

    for (i = 1; i <= 100; ++i) {
        result = e->compute(5, 7, 3);
        printf("step=repeat index=%d a=%d b=%d n=%d result=%d ", i, 5, 7, 3, result);
        print_state_fields(e);
        printf("\n");
    }

    printf("step=final ");
    print_state_fields(e);
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    Exports exports;
    int rc = load_exports(&exports);
    if (rc != 0) return rc;
    if (argc == 1) return run_legacy(&exports);
    if (argc == 2 && strcmp(argv[1], "stateful") == 0) return run_stateful(&exports);
    printf("usage: aevguest_ref.exe [stateful]\n");
    return 5;
}
