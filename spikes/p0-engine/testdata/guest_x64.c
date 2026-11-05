/*
 * P0-ENGINE-01 guest input (Windows x64, MSVC).
 *
 * This file is compiled to a COFF object.  The Aevotrace translator consumes
 * the .text bytes of that object; it does not recompile or rewrite this
 * source.  The functions deliberately exercise:
 *   - a loop and conditional branches
 *   - an array read and write
 *   - an indirect call through a function pointer argument
 *   - a variable multiply (imul), which is intentionally left out of the AOT
 *     subset to force the JIT hand-off.
 */
typedef int (*binop_t)(int, int);

__declspec(noinline) int guest_op(int x, int i) {
    return (x * 5) ^ (i + 0x33);
}

__declspec(noinline) int guest_kernel(int *arr, int n, int seed, binop_t op) {
    int acc = seed;
    for (int i = 0; i < n; ++i) {
        int v = arr[i];
        if (v & 1) {
            acc += op(v, i);      /* indirect call through the 4th argument */
        } else {
            acc += v * n;         /* variable multiply: imul */
        }
        acc ^= v << 1;
        if (acc < 0)
            acc = -acc;
    }
    arr[n & 7] = acc;             /* array write */
    return acc;
}
