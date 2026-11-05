#!/usr/bin/env bash
# P0-ENGINE-01 closed-loop checks.  Every check prints a PASS/FAIL line and the
# script exits non-zero if any check failed.
set -u

BUILD_DIR=${1:-"$(cd "$(dirname "$0")/../../.." && pwd)/.local/build-p0-engine"}
SRC_DIR=${2:-"$(cd "$(dirname "$0")/.." && pwd)"}
RESULTS=${AEV_RESULTS_DIR:-"$BUILD_DIR/results"}
RUNNER="$BUILD_DIR/aev-runner"
TRANSLATE="$BUILD_DIR/aev-translate"
DYLIB="$BUILD_DIR/generated/libguest_aot.dylib"
OBJ=${AEV_INPUT_OBJ:-"$SRC_DIR/testdata/guest_x64.obj"}
REFERENCE="$SRC_DIR/testdata/reference_stdout.txt"
CASES="prep_a prep_b hidden unaligned crosspage"

mkdir -p "$RESULTS/logs"
FAILURES=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }

run_runner() {
    local case_name=$1
    shift
    "$RUNNER" --aot "$DYLIB" --input "$OBJ" \
        --function guest_kernel --function guest_op \
        --case "$case_name" "$@"
}

echo "== environment =="
uname -a
cmake --version | head -1
clang --version | head -1
sw_vers 2>/dev/null || true
echo "BUILD_DIR=$BUILD_DIR"
echo "OBJ=$OBJ"
echo "DYLIB=$DYLIB"
shasum -a 256 "$OBJ" "$DYLIB" "$RUNNER" | tee "$RESULTS/hashes.txt"

echo "== ARM64 Mach-O checks =="
file "$BUILD_DIR/generated/guest_aot.o" "$DYLIB" "$RUNNER" | tee "$RESULTS/file.txt"
if grep -q "Mach-O 64-bit object arm64" "$RESULTS/file.txt" &&
   grep -q "Mach-O 64-bit dynamically linked shared library arm64" "$RESULTS/file.txt"; then
    pass "AOT object and dylib are ARM64 Mach-O"
else
    fail "AOT object/dylib are not the expected ARM64 Mach-O"
fi
otool -hv "$DYLIB" | tee "$RESULTS/otool_hv.txt" >/dev/null
otool -tv "$DYLIB" >"$RESULTS/otool_tv.txt"
nm -gU "$DYLIB" >"$RESULTS/nm_global.txt"
if grep -q "_aev_blk_140001000" "$RESULTS/otool_tv.txt" &&
   grep -q "_aev_blk_140002000" "$RESULTS/otool_tv.txt"; then
    pass "otool -tv shows translated guest block symbols in ARM64 text"
else
    fail "otool -tv does not show translated guest block symbols"
fi
if grep -q "_aev_block_count" "$RESULTS/nm_global.txt" &&
   grep -q "_aev_blocks" "$RESULTS/nm_global.txt" &&
   grep -q "_aev_aot_run" "$RESULTS/nm_global.txt"; then
    pass "dylib exports AOT entry, block table and count"
else
    fail "dylib export table is incomplete"
fi
if grep -q "_aev_blk_140001042" "$RESULTS/nm_global.txt"; then
    fail "imul block 0x140001042 leaked into the AOT table"
else
    pass "imul block 0x140001042 is absent from AOT symbols (JIT-only)"
fi

echo "== guest PC / block -> symbol mapping =="
run_runner prep_a --print-symbols >"$RESULTS/logs/symbols.stdout" 2>"$RESULTS/logs/symbols.stderr"
grep "^AOT block guest_rip=" "$RESULTS/logs/symbols.stdout" | tee "$RESULTS/block_symbols.txt"
if grep -q "guest_rip=0x140001000 symbol=aev_blk_140001000" "$RESULTS/block_symbols.txt" &&
   grep -q "guest_rip=0x140002000 symbol=aev_blk_140002000" "$RESULTS/block_symbols.txt"; then
    pass "guest PC 0x140001000/0x140002000 map to generated AOT symbols"
else
    fail "guest PC to AOT symbol mapping missing"
fi

echo "== closed-loop cases vs Windows MSVC reference =="
: >"$RESULTS/actual_stdout.txt"
for c in $CASES; do
    run_runner "$c" --trace --dump-jit >"$RESULTS/logs/$c.stdout" 2>"$RESULTS/logs/$c.stderr"
    rc=$?
    cat "$RESULTS/logs/$c.stdout" >>"$RESULTS/actual_stdout.txt"
    stats=$(grep '^STATS ' "$RESULTS/logs/$c.stderr" | tail -1)
    echo "$c: rc=$rc $stats" | tee -a "$RESULTS/summary.txt"
    if [ "$rc" -ne 0 ]; then
        fail "$c exited $rc"
        continue
    fi
    if ! grep -q "rc=0" <<<"$stats"; then
        fail "$c did not report rc=0 in stats"
        continue
    fi
    if ! grep -q "jit_compiles=1" <<<"$stats"; then
        fail "$c did not compile exactly one JIT block"
    fi
    if ! grep -q "jit_execs=[1-9]" <<<"$stats"; then
        fail "$c did not execute the JIT block"
    fi
    if ! grep -q "aot_blocks=[1-9]" <<<"$stats"; then
        fail "$c did not execute AOT blocks"
    fi
    pass "$c $(grep -o 'result=[-0-9]*' "$RESULTS/logs/$c.stdout" | head -1) stats-ok"
done
if diff -u "$REFERENCE" "$RESULTS/actual_stdout.txt" >"$RESULTS/reference.diff"; then
    pass "AOT+JIT output matches Windows MSVC reference for all cases"
else
    fail "AOT+JIT output differs from Windows reference (see reference.diff)"
    cat "$RESULTS/reference.diff"
fi
if [ "$(grep -c '^case=' "$RESULTS/actual_stdout.txt")" -eq 5 ]; then
    pass "all five cases produced output"
else
    fail "not all five cases produced output"
fi
if ! diff -q <(grep '^case=prep_a ' "$RESULTS/actual_stdout.txt") \
             <(grep '^case=hidden ' "$RESULTS/actual_stdout.txt") >/dev/null; then
    pass "held-out input changes the observed result (not fixed replay)"
else
    fail "held-out input produced the same result as prep_a"
fi

echo "== JIT provenance =="
grep -E "JIT compile|JIT exec|JIT return|JITCODE|JIT initialized" \
    "$RESULTS/logs/prep_a.stderr" >"$RESULTS/jit_evidence.txt" || true
cat "$RESULTS/jit_evidence.txt"
if grep -q "JIT compile rip=0x140001042" "$RESULTS/jit_evidence.txt" &&
   grep -q "JIT exec rip=0x140001042" "$RESULTS/jit_evidence.txt" &&
   grep -q "JITCODE 0x" "$RESULTS/jit_evidence.txt"; then
    pass "JIT block was compiled by ORC, its ARM64 code was dumped, and it executed"
else
    fail "JIT compile/execute/dump evidence missing"
fi
if grep -q "JIT return rip=0x140001042 next_rip=0x140001047" "$RESULTS/jit_evidence.txt"; then
    pass "JIT block returned to AOT successor 0x140001047"
else
    fail "JIT did not return to the AOT successor"
fi

echo "== AOT vs JIT distinction =="
if grep -q "^AOT  rip=0x140001000" "$RESULTS/logs/prep_a.stderr" &&
   grep -q "^AOT  rip=0x140002000" "$RESULTS/logs/prep_a.stderr" &&
   grep -q "JIT exec rip=0x140001042" "$RESULTS/logs/prep_a.stderr"; then
    pass "trace contains AOT entry, translated indirect-call target and JIT execution"
else
    fail "trace does not distinguish AOT entry/target and JIT execution"
fi

echo "== ASLR / independent process load =="
run_runner prep_a --print-symbols >"$RESULTS/logs/aslr1.stdout" 2>"$RESULTS/logs/aslr1.stderr"
run_runner prep_a --print-symbols >"$RESULTS/logs/aslr2.stdout" 2>"$RESULTS/logs/aslr2.stderr"
grep 'aot_load_base' "$RESULTS/logs/aslr1.stdout" "$RESULTS/logs/aslr2.stdout" | tee "$RESULTS/aslr.txt"
p1=$(grep -o 'aot_load_base=0x[0-9a-f]*' "$RESULTS/logs/aslr1.stdout" | head -1)
p2=$(grep -o 'aot_load_base=0x[0-9a-f]*' "$RESULTS/logs/aslr2.stdout" | head -1)
pid1=$(grep -o 'RUN pid=[0-9]*' "$RESULTS/logs/aslr1.stderr" | head -1)
pid2=$(grep -o 'RUN pid=[0-9]*' "$RESULTS/logs/aslr2.stderr" | head -1)
if [ "$pid1" != "$pid2" ]; then
    pass "two independent runner processes used ($pid1, $pid2)"
else
    fail "ASLR checks unexpectedly reused the same PID"
fi
if [ "$p1" != "$p2" ]; then
    pass "dylib loaded at different ASLR slide ($p1 vs $p2)"
else
    echo "WARN: ASLR slide was identical in this environment ($p1); dynamic load still exercised"
fi

echo "== explicit failure: unresolved indirect target =="
run_runner prep_a --bad-op >"$RESULTS/logs/badop.stdout" 2>"$RESULTS/logs/badop.stderr"
rc=$?
if [ "$rc" -ne 0 ] &&
   grep -q "unresolved guest indirect target rip=0xdead0000" "$RESULTS/logs/badop.stderr"; then
    pass "unresolved indirect target fails explicitly (rc=$rc)"
else
    fail "unresolved indirect target was not rejected explicitly (rc=$rc)"
fi

echo "== explicit failure: unsupported instructions =="
UNSUP="$RESULTS/unsupported_x64.obj"
clang --target=x86_64-pc-windows-msvc -O2 -c "$SRC_DIR/testdata/unsupported_x64.c" -o "$UNSUP"
"$TRANSLATE" --input "$UNSUP" --function divide_guest --entry divide_guest \
    --out-ll "$RESULTS/unsupported_div.ll" --out-header "$RESULTS/unsupported_div.h" \
    >"$RESULTS/logs/unsupported_div.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q "unsupported instruction" "$RESULTS/logs/unsupported_div.log"; then
    pass "cdq/idiv object rejected explicitly (rc=$rc)"
else
    fail "cdq/idiv object was not rejected explicitly (rc=$rc)"
fi
"$TRANSLATE" --input "$UNSUP" --function atomic_guest --entry atomic_guest \
    --out-ll "$RESULTS/unsupported_atomic.ll" --out-header "$RESULTS/unsupported_atomic.h" \
    >"$RESULTS/logs/unsupported_atomic.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q "unsupported instruction" "$RESULTS/logs/unsupported_atomic.log" &&
   grep -q "lock xadd" "$RESULTS/logs/unsupported_atomic.log"; then
    pass "atomic/lock instruction rejected explicitly (rc=$rc)"
else
    fail "atomic/lock instruction was not rejected explicitly (rc=$rc)"
fi

echo "== translator summary =="
cat "$BUILD_DIR/generated/guest_manifest.json" | tee "$RESULTS/guest_manifest.json" >/dev/null
if grep -q '"jit_block_count": 1' "$RESULTS/guest_manifest.json" &&
   grep -q '"jit_only": true' "$RESULTS/guest_manifest.json"; then
    pass "manifest records the single AOT coverage boundary (imul)"
else
    fail "manifest does not record the AOT coverage boundary"
fi

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL CHECKS PASSED"
    echo "results: $RESULTS"
    exit 0
else
    echo "$FAILURES CHECK(S) FAILED"
    echo "results: $RESULTS"
    exit 1
fi
