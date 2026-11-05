#!/usr/bin/env bash
# P0-ENGINE-AOT-EMPTY-01 regression.
#
# A function whose only block contains `imul` is JIT-only, so the AOT block
# table is empty.  The translator must still emit a legal LLVM module, and the
# resulting ARM64 dylib must load and hand the block to the JIT callback.  The
# checks below compile, link, load and execute the generated artifact; they do
# not merely inspect IR text.
#
# Usage: run_empty_aot_test.sh <build-dir> <source-dir>
set -u

BUILD_DIR=${1:-"$(cd "$(dirname "$0")/../../.." && pwd)/.local/build-core-fix-02"}
SRC_DIR=${2:-"$(cd "$(dirname "$0")/.." && pwd)"}
RESULTS=${AEV_RESULTS_DIR:-"$BUILD_DIR/results-empty-aot"}
LOGS="$RESULTS/logs"
SEM_SRC="$SRC_DIR/testdata/semantics"
OBJ="$RESULTS/obj"
AOT="$RESULTS/aot"

TRANSLATE="$BUILD_DIR/aev-translate"
PROBE="$BUILD_DIR/aev-aot-jit-probe"
RT_OBJ="$BUILD_DIR/generated/aot_runtime.o"
MEM_OBJ="$BUILD_DIR/generated/aev_mem_aot.o"

mkdir -p "$LOGS" "$OBJ" "$AOT"
: >"$RESULTS/summary.txt"
FAILURES=0
pass() { echo "PASS: $*" | tee -a "$RESULTS/summary.txt"; }
fail() { echo "FAIL: $*" | tee -a "$RESULTS/summary.txt"; FAILURES=$((FAILURES + 1)); }

need_tool() {
    if [ ! -x "$1" ]; then
        echo "FAIL: required tool not found: $1" | tee -a "$RESULTS/summary.txt"
        exit 1
    fi
}
need_tool "$TRANSLATE"
need_tool "$PROBE"
if [ ! -f "$RT_OBJ" ] || [ ! -f "$MEM_OBJ" ]; then
    echo "FAIL: AOT runtime objects are missing" | tee -a "$RESULTS/summary.txt"
    exit 1
fi

echo "== environment ==" | tee "$RESULTS/environment.txt"
{
    uname -a
    sw_vers 2>/dev/null || true
    clang --version | head -1
    echo "BUILD_DIR=$BUILD_DIR"
    echo "SRC=$SEM_SRC/empty_aot_imul.s"
    shasum -a 256 "$TRANSLATE" "$PROBE" "$RT_OBJ" "$MEM_OBJ"
} >>"$RESULTS/environment.txt" 2>&1

# Works around the shared helper names used by the other semantics script.
hexfield() { sed -n "s/.* $1=\(0x[0-9a-f][0-9a-f]*\).*/\1/p" "$2" | head -1; }
decfield() { sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p" "$2" | head -1; }
expect_hex() {
    local got
    got=$(hexfield "$3" "$2")
    if [ "$got" = "$4" ]; then pass "$1: $3=$got"; else fail "$1: $3 expected $4 got ${got:-<missing>}"; fi
}
expect_dec() {
    local got
    got=$(decfield "$3" "$2")
    if [ "$got" = "$4" ]; then pass "$1: $3=$got"; else fail "$1: $3 expected $4 got ${got:-<missing>}"; fi
}
expect_contains() {
    if grep -q "$3" "$2"; then pass "$1: contains '$3'"; else fail "$1: missing '$3' (see $2)"; fi
}

echo "== assemble zero-AOT-block input ==" | tee -a "$RESULTS/summary.txt"
if clang --target=x86_64-pc-windows-msvc -c "$SEM_SRC/empty_aot_imul.s" \
    -o "$OBJ/empty_aot_imul.obj" >"$LOGS/assemble.log" 2>&1; then
    pass "assembled empty_aot_imul.s (imul eax,ecx,7; ret)"
else
    fail "assemble empty_aot_imul.s"
    cat "$LOGS/assemble.log" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
fi
shasum -a 256 "$SEM_SRC/empty_aot_imul.s" "$OBJ/empty_aot_imul.obj" \
    >"$RESULTS/input.sha256"

LL="$AOT/empty_aot_imul.ll"
HDR="$AOT/empty_aot_imul.h"
MAN="$AOT/empty_aot_imul.json"
"$TRANSLATE" --input "$OBJ/empty_aot_imul.obj" --function probe --entry probe \
    --out-ll "$LL" --out-header "$HDR" --out-manifest "$MAN" \
    >"$LOGS/translate.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    pass "aev-translate rc=0 for JIT-only function"
else
    fail "aev-translate rc=$rc for JIT-only function"
    sed -n '1,8p' "$LOGS/translate.log" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
fi
expect_contains "translate summary" "$LOGS/translate.log" "aot_blocks=0 jit_blocks=1"
if grep -q '^@aev_blocks = constant \[0 x %BlockEntry\] zeroinitializer$' "$LL"; then
    pass "IR uses legal empty-table spelling 'zeroinitializer'"
else
    fail "IR does not contain the legal empty-table spelling"
fi
if grep -q '\[zeroinitializer\]' "$LL"; then
    fail "IR still contains the invalid '[zeroinitializer]' form"
else
    pass "IR does not contain the invalid '[zeroinitializer]' form"
fi
if grep -q '"aot_block_count": 0' "$MAN"; then
    pass "manifest records aot_block_count=0"
else
    fail "manifest does not record aot_block_count=0"
fi

echo "== compile and link the empty AOT table as ARM64 ==" | tee -a "$RESULTS/summary.txt"
clang --target=arm64-apple-macos14.0 -O2 -c "$LL" -o "$AOT/empty_aot_imul.o" \
    >"$LOGS/compile.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    pass "clang compiled the empty-table IR to ARM64"
else
    fail "clang compile rc=$rc"
    sed -n '1,8p' "$LOGS/compile.log" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
fi
clang -arch arm64 -dynamiclib -o "$AOT/empty_aot_imul.dylib" \
    "$AOT/empty_aot_imul.o" "$RT_OBJ" "$MEM_OBJ" >"$LOGS/link.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
    pass "linked empty-table ARM64 dylib with aot_runtime.o + aev_mem_aot.o"
else
    fail "link rc=$rc"
    sed -n '1,8p' "$LOGS/link.log" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
fi
if file "$AOT/empty_aot_imul.dylib" | grep -q "Mach-O 64-bit dynamically linked shared library arm64"; then
    pass "artifact is an ARM64 Mach-O dylib"
else
    fail "artifact is not an ARM64 Mach-O dylib"
fi
shasum -a 256 "$LL" "$AOT/empty_aot_imul.o" "$AOT/empty_aot_imul.dylib" \
    >"$RESULTS/aot-artifacts.sha256"

echo "== load and run through aev_aot_run -> jit_dispatch ==" | tee -a "$RESULTS/summary.txt"
OUT="$LOGS/empty_aot_probe.out"
ERR="$LOGS/empty_aot_probe.err"
"$PROBE" "$AOT/empty_aot_imul.dylib" "$OBJ/empty_aot_imul.obj" probe 0x140001000 \
    rax=10 rcx=3 >"$OUT" 2>"$ERR"
rc=$?
if [ "$rc" -eq 0 ]; then
    pass "aev-aot-jit-probe executed (rc=0)"
else
    fail "aev-aot-jit-probe failed (rc=$rc)"
    sed -n '1,8p' "$ERR" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
fi
expect_dec "empty AOT" "$OUT" rc 0
expect_hex "empty AOT" "$OUT" rax 0x15
expect_dec "empty AOT" "$OUT" error 0
expect_hex "empty AOT" "$OUT" rip_out 0xffffffffffffff00
expect_dec "empty AOT" "$OUT" aot_table_count 0
expect_dec "empty AOT" "$OUT" aot_blocks 0
expect_dec "empty AOT" "$OUT" jit_compiles 1
expect_dec "empty AOT" "$OUT" jit_execs 1
expect_dec "empty AOT" "$OUT" steps 1
expect_contains "empty AOT JIT" "$ERR" "JIT compile rip=0x140001000"
expect_contains "empty AOT JIT" "$ERR" "triple=arm64"
expect_contains "empty AOT JIT" "$ERR" "JIT exec rip=0x140001000"

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL EMPTY-AOT CHECKS PASSED"
    echo "results: $RESULTS"
    exit 0
else
    echo "$FAILURES EMPTY-AOT CHECK(S) FAILED"
    echo "results: $RESULTS"
    exit 1
fi
