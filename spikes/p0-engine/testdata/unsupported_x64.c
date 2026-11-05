/*
 * Negative-control source for the translator.  It is compiled locally with
 * clang --target=x86_64-pc-windows-msvc.  The resulting object contains
 * instructions outside the declared subset (cdq/idiv and lock xadd), and the
 * translator must reject it with an explicit diagnostic rather than emitting a
 * silent zero or a fabricated result.
 */
int divide_guest(int a, int b) { return a / b; }

int atomic_guest(int *p, int v) {
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}
