#!/usr/bin/env bash
# P0 callback bridge tests.  Runs the C bridge without GUI and checks the
# fixed-input identity, real AOT/JIT execution, Windows reference output,
# error paths and the 100-call lifecycle.
set -u

BUILD_DIR=${1:?build dir}
SRC_DIR=${2:?source dir}
RESULTS=${AEV_RESULTS_DIR:-"$BUILD_DIR/results-callback"}
TEST="$BUILD_DIR/aev_bridge_test"
COFF="$SRC_DIR/testdata/callback/aev_callback_fixture.obj"
AOT="$BUILD_DIR/generated/libcallback_aot.dylib"
MUTATED="$BUILD_DIR/generated/libcallback_aot_mutated.dylib"
REFERENCE="$SRC_DIR/testdata/callback/reference_stdout.txt"
mkdir -p "$RESULTS"
FAILURES=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }

echo "== environment =="
uname -a
cmake --version | head -1
clang --version | head -1
sw_vers 2>/dev/null || true
echo "BUILD_DIR=$BUILD_DIR"
echo "COFF=$COFF"
echo "AOT=$AOT"
shasum -a 256 "$COFF" "$AOT" "$MUTATED" "$TEST" | tee "$RESULTS/hashes.txt"

echo "== artifacts =="
file "$AOT" "$MUTATED" "$TEST" | tee "$RESULTS/file.txt"
if grep -q "Mach-O 64-bit dynamically linked shared library arm64" "$RESULTS/file.txt"; then
    pass "callback AOT dylib is ARM64 Mach-O"
else
    fail "callback AOT dylib is not ARM64 Mach-O"
fi
nm -gU "$AOT" >"$RESULTS/nm.txt"
if grep -q "_aev_aot_abi_version" "$RESULTS/nm.txt" &&
   grep -q "_aev_guest_state_size" "$RESULTS/nm.txt" &&
   grep -q "_aev_input_hash" "$RESULTS/nm.txt" &&
   grep -q "_aev_aot_run" "$RESULTS/nm.txt"; then
    pass "AOT dylib exports ABI/identity symbols"
else
    fail "AOT dylib ABI/identity symbols missing"
fi
if grep -q "_aev_blk_140001020" "$RESULTS/nm.txt"; then
    fail "JIT-only block leaked into the AOT table"
else
    pass "JIT-only block 0x140001020 absent from AOT table"
fi
otool -tv "$AOT" >"$RESULTS/otool_tv.txt"
if grep -q "_aev_blk_140001000" "$RESULTS/otool_tv.txt"; then
    pass "AOT entry block is present in ARM64 text"
else
    fail "AOT entry block missing from ARM64 text"
fi

echo "== bridge run =="
DYLD_LIBRARY_PATH="$BUILD_DIR:${DYLD_LIBRARY_PATH:-}" "$TEST" \
    --coff "$COFF" --aot "$AOT" --mutated-aot "$MUTATED" \
    >"$RESULTS/bridge.stdout" 2>"$RESULTS/bridge.stderr"
rc=$?
echo "bridge rc=$rc" | tee -a "$RESULTS/summary.txt"
cat "$RESULTS/bridge.stdout" | tee "$RESULTS/bridge_checks.txt"
if [ "$rc" -ne 0 ]; then
    fail "bridge test exited $rc"
fi
if grep -q "^FAILURES 0$" "$RESULTS/bridge.stdout"; then
    pass "bridge checks report zero failures"
else
    fail "bridge checks reported failures"
fi
grep '^CASE ' "$RESULTS/bridge.stdout" >"$RESULTS/bridge_cases.txt"
if diff -u "$REFERENCE" "$RESULTS/bridge_cases.txt" >"$RESULTS/reference.diff"; then
    pass "bridge CASE output matches Windows reference"
else
    fail "bridge CASE output differs from Windows reference (see reference.diff)"
    cat "$RESULTS/reference.diff"
fi
if grep -q "JIT compile rip=0x140001020" "$RESULTS/bridge.stderr" &&
   grep -q "JIT exec rip=0x140001020" "$RESULTS/bridge.stderr"; then
    pass "JIT block 0x140001020 actually compiled and executed"
else
    fail "JIT compile/exec evidence missing"
fi
if grep -q "CHECK alpha_aot PASS" "$RESULTS/bridge.stdout" &&
   grep -q "^AOT  rip=0x140001000" "$RESULTS/bridge.stderr" &&
   grep -q "^AOT  rip=0x14000101c" "$RESULTS/bridge.stderr"; then
    pass "generated AOT blocks actually executed"
else
    fail "AOT execution evidence missing"
fi
if grep -q "CHECK mutated_aot_rejected PASS" "$RESULTS/bridge.stdout"; then
    pass "mismatched AOT dylib rejected by input identity"
else
    fail "mismatched AOT dylib was not rejected"
fi

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL CALLBACK CHECKS PASSED"
    echo "results: $RESULTS"
    exit 0
fi
echo "$FAILURES CALLBACK CHECK(S) FAILED"
echo "results: $RESULTS"
exit 1
