/*
 * Windows x64 reference driver.  It is linked with the unmodified COFF object
 * produced from guest_x64.c and therefore executes the same machine code that
 * the macOS translator later consumes.
 *
 * Output is intentionally line-oriented and stable so it can be diffed against
 * the AOT/JIT runner output.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*binop_t)(int, int);

int guest_kernel(int *arr, int n, int seed, binop_t op);
int guest_op(int x, int i);

static int ref_op(int x, int i) {
    return (x * 5) ^ (i + 0x33);
}

static void run_case(const char *name, int *arr, int n, int seed) {
    int r = guest_kernel(arr, n, seed, ref_op);
    printf("case=%s result=%d arr=", name, r);
    for (int i = 0; i < 8; ++i) {
        int v;
        memcpy(&v, (const unsigned char *)arr + i * sizeof(int), sizeof(v));
        printf("%s%d", i ? "," : "", v);
    }
    printf("\n");
}

int main(void) {
    {
        int arr[8] = {11, -4, 7, 19, 0, -13, 21, 5};
        run_case("prep_a", arr, 8, (int)0x12345678u);
    }
    {
        int arr[8] = {-9, 2, -3, 4, 5, -6, 7, 8};
        run_case("prep_b", arr, 6, (int)0x01020304u);
    }
    {
        int arr[8] = {100, -101, 102, -103, 104, -105, 106, -107};
        run_case("hidden", arr, 7, (int)0x0BADF00Du);
    }
    {
        unsigned char buf[64];
        int init[8] = {13, -8, 21, 0, -5, 34, 55, -89};
        memcpy(buf + 1, init, sizeof(init));
        run_case("unaligned", (int *)(void *)(buf + 1), 7, (int)0x55AA33CCu);
    }
    {
        unsigned char *buf = (unsigned char *)_aligned_malloc(3 * 4096, 4096);
        if (!buf)
            return 2;
        int init[8] = {17, -22, 31, -44, 53, -66, 77, -88};
        memcpy(buf + 4096 - 6, init, sizeof(init));
        run_case("crosspage", (int *)(void *)(buf + 4096 - 6), 4, (int)0x0F1E2D3Cu);
        _aligned_free(buf);
    }
    return 0;
}
