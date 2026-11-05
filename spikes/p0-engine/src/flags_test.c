/* Direct IMUL flag-helper regression.  Runs the host-compiled helper by
 * default, or the helper exported by a generated AOT dylib with
 * `--dylib <libguest_aot.dylib>`.  Only defined CF/OF are judged; ZF/SF/PF
 * are architecturally undefined for IMUL. */
#include "aev_guest.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*ImulFn)(GuestState *, uint32_t, uint64_t, uint64_t, uint64_t);

struct Case {
    const char *name;
    uint32_t width;
    uint64_t a;
    uint64_t b;
    uint64_t r;
    int cf;
    int of;
};

static const struct Case kCases[] = {
    /* 64-bit */
    {"w8_neg1_x_1", 8, UINT64_MAX, 1, UINT64_MAX, 0, 0},
    {"w8_min_x_1", 8, 0x8000000000000000ULL, 1, 0x8000000000000000ULL, 0, 0},
    {"w8_max_x_1", 8, 0x7fffffffffffffffULL, 1, 0x7fffffffffffffffULL, 0, 0},
    {"w8_min_x_neg1", 8, 0x8000000000000000ULL, UINT64_MAX, 0x8000000000000000ULL, 1, 1},
    {"w8_max_x_2", 8, 0x7fffffffffffffffULL, 2, 0xfffffffffffffffeULL, 1, 1},
    {"w8_2p62_x_2", 8, 0x4000000000000000ULL, 2, 0x8000000000000000ULL, 1, 1},
    {"w8_neg2_x_max", 8, UINT64_MAX - 1, 0x7fffffffffffffffULL, 2, 1, 1},
    {"w8_neg1_x_neg1", 8, UINT64_MAX, UINT64_MAX, 1, 0, 0},
    {"w8_min_x_min", 8, 0x8000000000000000ULL, 0x8000000000000000ULL, 0, 1, 1},
    {"w8_zero_x_min", 8, 0, 0x8000000000000000ULL, 0, 0, 0},
    /* 32-bit */
    {"w4_neg1_x_1", 4, 0xffffffffULL, 1, 0xffffffffULL, 0, 0},
    {"w4_min_x_neg1", 4, 0x80000000ULL, 0xffffffffULL, 0x80000000ULL, 1, 1},
    {"w4_max_x_2", 4, 0x7fffffffULL, 2, 0xfffffffeULL, 1, 1},
    {"w4_2p30_x_2", 4, 0x40000000ULL, 2, 0x80000000ULL, 1, 1},
    {"w4_neg1_x_neg1", 4, 0xffffffffULL, 0xffffffffULL, 1, 0, 0},
    {"w4_min_x_min", 4, 0x80000000ULL, 0x80000000ULL, 0, 1, 1},
    /* 16-bit */
    {"w2_neg1_x_1", 2, 0xffffULL, 1, 0xffffULL, 0, 0},
    {"w2_min_x_neg1", 2, 0x8000ULL, 0xffffULL, 0x8000ULL, 1, 1},
    {"w2_max_x_2", 2, 0x7fffULL, 2, 0xfffeULL, 1, 1},
    {"w2_2p14_x_2", 2, 0x4000ULL, 2, 0x8000ULL, 1, 1},
    {"w2_neg1_x_neg1", 2, 0xffffULL, 0xffffULL, 1, 0, 0},
    /* 8-bit */
    {"w1_neg1_x_1", 1, 0xffULL, 1, 0xffULL, 0, 0},
    {"w1_min_x_neg1", 1, 0x80ULL, 0xffULL, 0x80ULL, 1, 1},
    {"w1_max_x_2", 1, 0x7fULL, 2, 0xfeULL, 1, 1},
    {"w1_2p6_x_2", 1, 0x40ULL, 2, 0x80ULL, 1, 1},
    {"w1_neg1_x_neg1", 1, 0xffULL, 0xffULL, 1, 0, 0},
    {"w1_min_x_min", 1, 0x80ULL, 0x80ULL, 0, 1, 1},
};

int main(int argc, char **argv) {
    ImulFn fn = aev_flags_imul;
    void *handle = NULL;
    if (argc == 3 && strcmp(argv[1], "--dylib") == 0) {
        handle = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            fprintf(stderr, "dlopen failed: %s\n", dlerror());
            return 2;
        }
        fn = (ImulFn)dlsym(handle, "aev_flags_imul");
        if (!fn) {
            fprintf(stderr, "dlsym(aev_flags_imul) failed: %s\n", dlerror());
            return 2;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--dylib <libguest_aot.dylib>]\n", argv[0]);
        return 2;
    }

    int failures = 0;
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        const struct Case *c = &kCases[i];
        GuestState s;
        memset(&s, 0, sizeof(s));
        /* Pre-set CF/OF so a no-overflow case proves they are cleared. */
        s.rflags = AEV_CF | AEV_OF;
        fn(&s, c->width, c->a, c->b, c->r);
        int cf = !!(s.rflags & AEV_CF);
        int of = !!(s.rflags & AEV_OF);
        int ok = (cf == c->cf && of == c->of);
        printf("%s %s width=%u a=0x%llx b=0x%llx r=0x%llx CF=%d OF=%d expected=%d/%d\n",
               ok ? "PASS" : "FAIL", c->name, c->width,
               (unsigned long long)c->a, (unsigned long long)c->b,
               (unsigned long long)c->r, cf, of, c->cf, c->of);
        if (!ok) failures++;
    }
    if (handle) dlclose(handle);
    printf("flags_test failures=%d cases=%zu\n", failures,
           sizeof(kCases) / sizeof(kCases[0]));
    return failures == 0 ? 0 : 1;
}
