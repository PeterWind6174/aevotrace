#!/usr/bin/env bash
# P0-CORE-FIX-01 instruction-semantics regressions.
#
# The checks are deliberately execution-based:
#   * F1 is rejected before IR generation (translator) and via the JIT path.
#   * F2/F5 execute the ARM64 code produced by LLVM ORC.
#   * F3 is executed through both the JIT path and a separately loaded AOT
#     dylib, including the exact error_pc and non-committed side effects.
#   * F4 executes POP RSP through JIT and AOT and rejects 16-bit stack forms.
#   * F5 runs the flag helper directly, against the AOT dylib, and through JIT.
#
# Usage: run_semantics_tests.sh <build-dir> <source-dir>
set -u

BUILD_DIR=${1:-"$(cd "$(dirname "$0")/../../.." && pwd)/.local/build-core-fix"}
SRC_DIR=${2:-"$(cd "$(dirname "$0")/.." && pwd)"}
RESULTS=${AEV_RESULTS_DIR:-"$BUILD_DIR/results-core-fix"}
LOGS="$RESULTS/logs"
SEM_SRC="$SRC_DIR/testdata/semantics"
OBJ="$BUILD_DIR/semantics/obj"
AOT="$BUILD_DIR/semantics/aot"

TRANSLATE="$BUILD_DIR/aev-translate"
JIT_PROBE="$BUILD_DIR/aev-jit-probe"
AOT_PROBE="$BUILD_DIR/aev-aot-probe"
FLAGS_TEST="$BUILD_DIR/aev-flags-test"
AOT_DYLIB=${AEV_AOT_DYLIB:-"$BUILD_DIR/generated/libguest_aot.dylib"}

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
need_tool "$JIT_PROBE"
need_tool "$AOT_PROBE"
need_tool "$FLAGS_TEST"
if [ ! -f "$AOT_DYLIB" ]; then
    echo "FAIL: AOT dylib not found: $AOT_DYLIB" | tee -a "$RESULTS/summary.txt"
    exit 1
fi
if [ ! -f "$BUILD_DIR/generated/aot_runtime.o" ] ||
   [ ! -f "$BUILD_DIR/generated/aev_mem_aot.o" ]; then
    echo "FAIL: AOT runtime objects are missing" | tee -a "$RESULTS/summary.txt"
    exit 1
fi

echo "== environment ==" | tee "$RESULTS/environment.txt"
{
    uname -a
    sw_vers 2>/dev/null || true
    clang --version | head -1
    echo "BUILD_DIR=$BUILD_DIR"
    echo "SEM_SRC=$SEM_SRC"
    echo "AOT_DYLIB=$AOT_DYLIB"
    shasum -a 256 "$AOT_DYLIB" "$JIT_PROBE" "$AOT_PROBE" "$FLAGS_TEST" "$TRANSLATE"
} >>"$RESULTS/environment.txt" 2>&1

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
hexfield() { # key file
    sed -n "s/.* $1=\(0x[0-9a-f][0-9a-f]*\).*/\1/p" "$2" | head -1
}
decfield() { # key file
    sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p" "$2" | head -1
}
expect_hex() { # label file key expected
    local got
    got=$(hexfield "$3" "$2")
    if [ "$got" = "$4" ]; then
        pass "$1: $3=$got"
    else
        fail "$1: $3 expected $4 got ${got:-<missing>}"
    fi
}
expect_dec() { # label file key expected
    local got
    got=$(decfield "$3" "$2")
    if [ "$got" = "$4" ]; then
        pass "$1: $3=$got"
    else
        fail "$1: $3 expected $4 got ${got:-<missing>}"
    fi
}
expect_contains() { # label file needle
    if grep -q "$3" "$2"; then
        pass "$1: contains '$3'"
    else
        fail "$1: missing '$3' (see $2)"
    fi
}
expect_nonzero() { # label rc
    if [ "$2" -ne 0 ]; then
        pass "$1: rejected (rc=$2)"
    else
        fail "$1: unexpectedly succeeded"
    fi
}

translate_fail() { # name needle...
    local name=$1
    shift
    local out="$LOGS/$name.translate.log"
    "$TRANSLATE" --input "$OBJ/$name.obj" --function probe --entry probe \
        --out-ll "$RESULTS/$name.ll" --out-header "$RESULTS/$name.h" \
        >"$out" 2>&1
    local rc=$?
    local ok=1
    [ "$rc" -ne 0 ] || ok=0
    local needle
    for needle in "$@"; do
        grep -q "$needle" "$out" || ok=0
    done
    if [ "$ok" -eq 1 ]; then
        pass "translate $name rejected with $(echo "$*" | tr ' ' ',') (rc=$rc)"
    else
        fail "translate $name not rejected with expected message (rc=$rc)"
        sed -n '1,5p' "$out" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
    fi
}

jit_run() { # label object args...
    local label=$1 obj=$2
    shift 2
    "$JIT_PROBE" "$OBJ/$obj.obj" probe 0x140001000 "$@" \
        >"$LOGS/$label.jit.out" 2>"$LOGS/$label.jit.err"
    JIT_RC=$?
    JIT_OUT="$LOGS/$label.jit.out"
    JIT_ERR="$LOGS/$label.jit.err"
}

jit_ok() { # label
    local label=$1
    if [ "$JIT_RC" -eq 0 ]; then
        pass "$label: JIT path executed"
    else
        fail "$label: JIT path failed rc=$JIT_RC"
        sed -n '1,5p' "$JIT_ERR" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
        return 1
    fi
    if grep -q "JIT compile .* triple=arm64" "$JIT_ERR"; then
        pass "$label: ORC emitted ARM64 (JIT compile ... triple=arm64)"
    else
        fail "$label: missing ORC/ARM64 compile evidence"
    fi
}

jit_reject() { # label object needle
    local label=$1 obj=$2 needle=$3
    "$JIT_PROBE" "$OBJ/$obj.obj" probe 0x140001000 \
        >"$LOGS/$label.jit.out" 2>"$LOGS/$label.jit.err"
    local rc=$?
    if [ "$rc" -ne 0 ] && grep -q "$needle" "$LOGS/$label.jit.err"; then
        pass "$label: JIT path rejected with '$needle' (rc=$rc)"
    else
        fail "$label: JIT path did not reject with '$needle' (rc=$rc)"
        sed -n '1,5p' "$LOGS/$label.jit.err" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
    fi
}

build_aot() { # name
    local name=$1
    local tr="$LOGS/aot-$name.translate.log"
    local cc="$LOGS/aot-$name.compile.log"
    local ld="$LOGS/aot-$name.link.log"
    "$TRANSLATE" --input "$OBJ/$name.obj" --function probe --entry probe \
        --out-ll "$AOT/$name.ll" --out-header "$AOT/$name.h" >"$tr" 2>&1
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        fail "AOT translate $name (rc=$rc)"
        sed -n '1,5p' "$tr" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
        return 1
    fi
    clang --target=arm64-apple-macos14.0 -O2 -c "$AOT/$name.ll" -o "$AOT/$name.o" \
        >"$cc" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        fail "AOT compile $name (rc=$rc)"
        sed -n '1,8p' "$cc" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
        return 1
    fi
    clang -arch arm64 -dynamiclib -o "$AOT/$name.dylib" "$AOT/$name.o" \
        "$BUILD_DIR/generated/aot_runtime.o" "$BUILD_DIR/generated/aev_mem_aot.o" \
        >"$ld" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        fail "AOT link $name (rc=$rc)"
        sed -n '1,8p' "$ld" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
        return 1
    fi
    shasum -a 256 "$AOT/$name.ll" "$AOT/$name.o" "$AOT/$name.dylib" \
        >>"$RESULTS/aot-artifacts.sha256"
    pass "AOT artifact built for $name"
}

aot_run() { # label name args...
    local label=$1 name=$2
    shift 2
    if [ ! -f "$AOT/$name.dylib" ]; then
        JIT_RC=2
        AOT_RC=2
        AOT_OUT="$LOGS/$label.aot.out"
        : >"$AOT_OUT"
        fail "$label: missing $AOT/$name.dylib"
        return
    fi
    "$AOT_PROBE" "$AOT/$name.dylib" 0x140001000 "$@" \
        >"$LOGS/$label.aot.out" 2>"$LOGS/$label.aot.err"
    AOT_RC=$?
    AOT_OUT="$LOGS/$label.aot.out"
}

fault_common() { # label file pc
    local label=$1 file=$2 pc=$3
    expect_dec "$label" "$file" error 1
    expect_hex "$label" "$file" error_pc "$pc"
    expect_hex "$label" "$file" rip_out "$pc"
    local rin rout
    rin=$(hexfield rsp_in "$file")
    rout=$(hexfield rsp "$file")
    if [ -n "$rin" ] && [ "$rin" = "$rout" ]; then
        pass "$label: RSP unchanged (rsp=rsp_in=$rin)"
    else
        fail "$label: RSP changed (rsp_in=${rin:-?} rsp=${rout:-?})"
    fi
}

flags_of() { # file -> "cf/of"
    local file=$1
    local line
    line=$(grep '^CF=' "$file" | tail -1)
    local cf of
    cf=$(sed -n 's/^CF=\([0-9]\).*/\1/p' <<<"$line")
    of=$(sed -n 's/^CF=[0-9] OF=\([0-9]\).*/\1/p' <<<"$line")
    echo "$cf/$of"
}

expect_flags() { # label file cf of
    local got
    got=$(flags_of "$2")
    if [ "$got" = "$3/$4" ]; then
        pass "$1: CF/OF=$got"
    else
        fail "$1: CF/OF expected $3/$4 got ${got:-<missing>}"
    fi
}

# ---------------------------------------------------------------------------
# assemble the committed x86-64 counterexamples
# ---------------------------------------------------------------------------
echo "== assemble semantics inputs ==" | tee -a "$RESULTS/summary.txt"
assemble_ok=1
for s in "$SEM_SRC"/*.s; do
    name=$(basename "$s" .s)
    if ! clang --target=x86_64-pc-windows-msvc -c "$s" -o "$OBJ/$name.obj" \
        >"$LOGS/assemble-$name.log" 2>&1; then
        fail "assemble $name"
        assemble_ok=0
    fi
done
if [ "$assemble_ok" -eq 1 ]; then
    pass "assembled $(ls "$SEM_SRC"/*.s | wc -l | tr -d ' ') x86-64 COFF inputs"
else
    echo "FAIL: input assembly failed; stopping" | tee -a "$RESULTS/summary.txt"
    exit 1
fi
shasum -a 256 "$SEM_SRC"/*.s >"$RESULTS/input-sources.sha256"
shasum -a 256 "$OBJ"/*.obj >"$RESULTS/input-objects.sha256"
: >"$RESULTS/aot-artifacts.sha256"

# ---------------------------------------------------------------------------
# F1: LOCK prefix is rejected, not lowered as a plain load/store
# ---------------------------------------------------------------------------
echo "== F1 LOCK prefix ==" | tee -a "$RESULTS/summary.txt"
for entry in \
    "lock_add:add" "lock_addq:add" "lock_addw:add" "lock_addb:add" \
    "lock_inc:inc" "lock_and:and" "lock_or:or" "lock_xor:xor" \
    "lock_sub:sub" "lock_dec:dec"; do
    name=${entry%%:*}
    mnemonic=${entry##*:}
    translate_fail "$name" "unsupported instruction" "LOCK" "lock $mnemonic" "0x140001000"
    jit_reject "F1_$name" "$name" "LOCK"
done

# ---------------------------------------------------------------------------
# F2: two/three-operand IMUL; the JIT must execute the ARM64 code
# ---------------------------------------------------------------------------
echo "== F2 IMUL ==" | tee -a "$RESULTS/summary.txt"
jit_run F2_imul3_32 imul3_32 rax=10 rcx=3
if jit_ok F2_imul3_32; then expect_hex F2_imul3_32 "$JIT_OUT" rax 0x15; fi
jit_run F2_imul2_32 imul2_32 rax=10 rcx=3
if jit_ok F2_imul2_32; then expect_hex F2_imul2_32 "$JIT_OUT" rax 0x1e; fi
jit_run F2_imul3_32_neg imul3_32_neg rax=10 rcx=3
if jit_ok F2_imul3_32_neg; then expect_hex F2_imul3_32_neg "$JIT_OUT" rax 0xfffffff7; fi
jit_run F2_imul3_64 imul3_64 rax=10 rcx=3
if jit_ok F2_imul3_64; then expect_hex F2_imul3_64 "$JIT_OUT" rax 0x15; fi
jit_run F2_imul2_64 imul2_64 rax=10 rcx=3
if jit_ok F2_imul2_64; then expect_hex F2_imul2_64 "$JIT_OUT" rax 0x1e; fi
jit_run F2_imul3_64_neg imul3_64_neg rax=10 rcx=3
if jit_ok F2_imul3_64_neg; then expect_hex F2_imul3_64_neg "$JIT_OUT" rax 0xfffffffffffffff7; fi
jit_run F2_imul3_16 imul3_16 rax=10 rcx=3
if jit_ok F2_imul3_16; then expect_hex F2_imul3_16 "$JIT_OUT" rax 0x15; fi
jit_run F2_imul2_16 imul2_16 rax=10 rcx=3
if jit_ok F2_imul2_16; then expect_hex F2_imul2_16 "$JIT_OUT" rax 0x1e; fi
jit_run F2_imul3_16_neg imul3_16_neg rax=10 rcx=3
if jit_ok F2_imul3_16_neg; then expect_hex F2_imul3_16_neg "$JIT_OUT" rax 0xfff7; fi
# The only 8-bit IMUL form is one-operand; it must be reported, not mis-lowered.
translate_fail imul1_8 "one-operand imul" "0x140001000"
jit_reject F2_imul1_8 imul1_8 "one-operand imul"

# ---------------------------------------------------------------------------
# F3: memory faults stop the block; no later or faulting side effects commit
# ---------------------------------------------------------------------------
echo "== F3 memory-fault side effects ==" | tee -a "$RESULTS/summary.txt"

# Faulting load is the second instruction; error_pc must be 0x140001005.
jit_run F3_jit_fault_load_second fault_load_second \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rcx=0 \
    rdx=mem+0x200 mem=mem+0x200:8:0 dump=mem+0x200:8
if jit_ok F3_jit_fault_load_second; then
    fault_common F3_jit_fault_load_second "$JIT_OUT" 0x140001005
    expect_hex F3_jit_fault_load_second "$JIT_OUT" rax 0xaaaaaaaaaaaaaaaa
    expect_hex F3_jit_fault_load_second "$JIT_OUT" rbx 0x11223344
    expect_contains F3_jit_fault_load_second "$JIT_OUT" "bytes=00:00:00:00:00:00:00:00"
fi
build_aot fault_load_second
aot_run F3_aot_fault_load_second fault_load_second \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rcx=0 \
    rdx=mem+0x200 mem=mem+0x200:8:0 dump=mem+0x200:8
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_load_second "$AOT_OUT" rc 1
    fault_common F3_aot_fault_load_second "$AOT_OUT" 0x140001005
    expect_hex F3_aot_fault_load_second "$AOT_OUT" rax 0xaaaaaaaaaaaaaaaa
    expect_hex F3_aot_fault_load_second "$AOT_OUT" rbx 0x11223344
    expect_contains F3_aot_fault_load_second "$AOT_OUT" "bytes=00:00:00:00:00:00:00:00"
else
    fail "F3_aot_fault_load_second: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_load_first fault_load_first \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rcx=0 \
    rdx=mem+0x200 mem=mem+0x200:8:0 dump=mem+0x200:8
if jit_ok F3_jit_fault_load_first; then
    fault_common F3_jit_fault_load_first "$JIT_OUT" 0x140001000
    expect_hex F3_jit_fault_load_first "$JIT_OUT" rax 0xaaaaaaaaaaaaaaaa
    expect_contains F3_jit_fault_load_first "$JIT_OUT" "bytes=00:00:00:00:00:00:00:00"
fi
build_aot fault_load_first
aot_run F3_aot_fault_load_first fault_load_first \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rcx=0 \
    rdx=mem+0x200 mem=mem+0x200:8:0 dump=mem+0x200:8
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_load_first "$AOT_OUT" rc 1
    fault_common F3_aot_fault_load_first "$AOT_OUT" 0x140001000
    expect_hex F3_aot_fault_load_first "$AOT_OUT" rax 0xaaaaaaaaaaaaaaaa
    expect_contains F3_aot_fault_load_first "$AOT_OUT" "bytes=00:00:00:00:00:00:00:00"
else
    fail "F3_aot_fault_load_first: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_store_second fault_store_second \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rcx=0xcccccccccccccccc \
    rdx=mem+0x80000
if jit_ok F3_jit_fault_store_second; then
    fault_common F3_jit_fault_store_second "$JIT_OUT" 0x14000100a
    expect_hex F3_jit_fault_store_second "$JIT_OUT" rax 0x11
    expect_hex F3_jit_fault_store_second "$JIT_OUT" rcx 0x22
    expect_hex F3_jit_fault_store_second "$JIT_OUT" rbx 0xbbbbbbbbbbbbbbbb
fi
build_aot fault_store_second
aot_run F3_aot_fault_store_second fault_store_second \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rcx=0xcccccccccccccccc \
    rdx=mem+0x80000
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_store_second "$AOT_OUT" rc 1
    fault_common F3_aot_fault_store_second "$AOT_OUT" 0x14000100a
    expect_hex F3_aot_fault_store_second "$AOT_OUT" rax 0x11
    expect_hex F3_aot_fault_store_second "$AOT_OUT" rcx 0x22
    expect_hex F3_aot_fault_store_second "$AOT_OUT" rbx 0xbbbbbbbbbbbbbbbb
else
    fail "F3_aot_fault_store_second: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_store fault_store \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rdx=mem+0x80000
if jit_ok F3_jit_fault_store; then
    fault_common F3_jit_fault_store "$JIT_OUT" 0x140001005
    expect_hex F3_jit_fault_store "$JIT_OUT" rax 0x55
    expect_hex F3_jit_fault_store "$JIT_OUT" rbx 0xbbbbbbbbbbbbbbbb
fi
build_aot fault_store
aot_run F3_aot_fault_store fault_store \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rdx=mem+0x80000
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_store "$AOT_OUT" rc 1
    fault_common F3_aot_fault_store "$AOT_OUT" 0x140001005
    expect_hex F3_aot_fault_store "$AOT_OUT" rax 0x55
    expect_hex F3_aot_fault_store "$AOT_OUT" rbx 0xbbbbbbbbbbbbbbbb
else
    fail "F3_aot_fault_store: probe failed rc=$AOT_RC"
fi

# A store crossing the end of the region must not write even its in-range part.
jit_run F3_jit_fault_store_cross fault_store_cross \
    rdx=mem_end-2 mem=mem_end-2:2:0 dump=mem_end-2:2
if jit_ok F3_jit_fault_store_cross; then
    fault_common F3_jit_fault_store_cross "$JIT_OUT" 0x140001000
    expect_contains F3_jit_fault_store_cross "$JIT_OUT" "bytes=00:00"
fi
build_aot fault_store_cross
aot_run F3_aot_fault_store_cross fault_store_cross \
    rdx=mem_end-2 mem=mem_end-2:2:0 dump=mem_end-2:2
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_store_cross "$AOT_OUT" rc 1
    fault_common F3_aot_fault_store_cross "$AOT_OUT" 0x140001000
    expect_contains F3_aot_fault_store_cross "$AOT_OUT" "bytes=00:00"
else
    fail "F3_aot_fault_store_cross: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_push fault_push \
    rax=0x1234 rbx=0xbbbbbbbbbbbbbbbb rsp=0
if jit_ok F3_jit_fault_push; then
    fault_common F3_jit_fault_push "$JIT_OUT" 0x140001000
    expect_hex F3_jit_fault_push "$JIT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_jit_fault_push "$JIT_OUT" rsp 0x0
fi
build_aot fault_push
aot_run F3_aot_fault_push fault_push \
    rax=0x1234 rbx=0xbbbbbbbbbbbbbbbb rsp=0
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_push "$AOT_OUT" rc 1
    fault_common F3_aot_fault_push "$AOT_OUT" 0x140001000
    expect_hex F3_aot_fault_push "$AOT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_aot_fault_push "$AOT_OUT" rsp 0x0
else
    fail "F3_aot_fault_push: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_pop fault_pop \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rsp=0
if jit_ok F3_jit_fault_pop; then
    fault_common F3_jit_fault_pop "$JIT_OUT" 0x140001000
    expect_hex F3_jit_fault_pop "$JIT_OUT" rax 0xaaaaaaaaaaaaaaaa
    expect_hex F3_jit_fault_pop "$JIT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_jit_fault_pop "$JIT_OUT" rsp 0x0
fi
build_aot fault_pop
aot_run F3_aot_fault_pop fault_pop \
    rax=0xaaaaaaaaaaaaaaaa rbx=0xbbbbbbbbbbbbbbbb rsp=0
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_pop "$AOT_OUT" rc 1
    fault_common F3_aot_fault_pop "$AOT_OUT" 0x140001000
    expect_hex F3_aot_fault_pop "$AOT_OUT" rax 0xaaaaaaaaaaaaaaaa
    expect_hex F3_aot_fault_pop "$AOT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_aot_fault_pop "$AOT_OUT" rsp 0x0
else
    fail "F3_aot_fault_pop: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_ret fault_ret rax=0xaaaaaaaaaaaaaaaa rsp=0
if jit_ok F3_jit_fault_ret; then
    fault_common F3_jit_fault_ret "$JIT_OUT" 0x140001000
    expect_hex F3_jit_fault_ret "$JIT_OUT" rax 0xaaaaaaaaaaaaaaaa
    expect_hex F3_jit_fault_ret "$JIT_OUT" rsp 0x0
fi
build_aot fault_ret
aot_run F3_aot_fault_ret fault_ret rax=0xaaaaaaaaaaaaaaaa rsp=0
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_ret "$AOT_OUT" rc 1
    fault_common F3_aot_fault_ret "$AOT_OUT" 0x140001000
    expect_hex F3_aot_fault_ret "$AOT_OUT" rax 0xaaaaaaaaaaaaaaaa
    expect_hex F3_aot_fault_ret "$AOT_OUT" rsp 0x0
else
    fail "F3_aot_fault_ret: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_call fault_call rbx=0xbbbbbbbbbbbbbbbb rsp=0
if jit_ok F3_jit_fault_call; then
    fault_common F3_jit_fault_call "$JIT_OUT" 0x140001000
    expect_hex F3_jit_fault_call "$JIT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_jit_fault_call "$JIT_OUT" rsp 0x0
    expect_hex F3_jit_fault_call "$JIT_OUT" rip_out 0x140001000
fi
build_aot fault_call
aot_run F3_aot_fault_call fault_call rbx=0xbbbbbbbbbbbbbbbb rsp=0
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_call "$AOT_OUT" rc 1
    fault_common F3_aot_fault_call "$AOT_OUT" 0x140001000
    expect_hex F3_aot_fault_call "$AOT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_aot_fault_call "$AOT_OUT" rsp 0x0
    expect_hex F3_aot_fault_call "$AOT_OUT" rip_out 0x140001000
else
    fail "F3_aot_fault_call: probe failed rc=$AOT_RC"
fi

# The stack faults below are the second instruction, so the exact-PC check
# cannot be satisfied by the block entry address.
jit_run F3_jit_fault_push_second fault_push_second rbx=0xbbbbbbbbbbbbbbbb rsp=0
if jit_ok F3_jit_fault_push_second; then
    fault_common F3_jit_fault_push_second "$JIT_OUT" 0x140001005
    expect_hex F3_jit_fault_push_second "$JIT_OUT" rax 0x11
    expect_hex F3_jit_fault_push_second "$JIT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_jit_fault_push_second "$JIT_OUT" rsp 0x0
fi
build_aot fault_push_second
aot_run F3_aot_fault_push_second fault_push_second rbx=0xbbbbbbbbbbbbbbbb rsp=0
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_push_second "$AOT_OUT" rc 1
    fault_common F3_aot_fault_push_second "$AOT_OUT" 0x140001005
    expect_hex F3_aot_fault_push_second "$AOT_OUT" rax 0x11
    expect_hex F3_aot_fault_push_second "$AOT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_aot_fault_push_second "$AOT_OUT" rsp 0x0
else
    fail "F3_aot_fault_push_second: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_pop_second fault_pop_second rbx=0xbbbbbbbbbbbbbbbb rsp=0
if jit_ok F3_jit_fault_pop_second; then
    fault_common F3_jit_fault_pop_second "$JIT_OUT" 0x140001005
    expect_hex F3_jit_fault_pop_second "$JIT_OUT" rax 0x11
    expect_hex F3_jit_fault_pop_second "$JIT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_jit_fault_pop_second "$JIT_OUT" rsp 0x0
fi
build_aot fault_pop_second
aot_run F3_aot_fault_pop_second fault_pop_second rbx=0xbbbbbbbbbbbbbbbb rsp=0
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_pop_second "$AOT_OUT" rc 1
    fault_common F3_aot_fault_pop_second "$AOT_OUT" 0x140001005
    expect_hex F3_aot_fault_pop_second "$AOT_OUT" rax 0x11
    expect_hex F3_aot_fault_pop_second "$AOT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_aot_fault_pop_second "$AOT_OUT" rsp 0x0
else
    fail "F3_aot_fault_pop_second: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_ret_second fault_ret_second rsp=0
if jit_ok F3_jit_fault_ret_second; then
    fault_common F3_jit_fault_ret_second "$JIT_OUT" 0x140001005
    expect_hex F3_jit_fault_ret_second "$JIT_OUT" rax 0x11
    expect_hex F3_jit_fault_ret_second "$JIT_OUT" rsp 0x0
fi
build_aot fault_ret_second
aot_run F3_aot_fault_ret_second fault_ret_second rsp=0
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_ret_second "$AOT_OUT" rc 1
    fault_common F3_aot_fault_ret_second "$AOT_OUT" 0x140001005
    expect_hex F3_aot_fault_ret_second "$AOT_OUT" rax 0x11
    expect_hex F3_aot_fault_ret_second "$AOT_OUT" rsp 0x0
else
    fail "F3_aot_fault_ret_second: probe failed rc=$AOT_RC"
fi

jit_run F3_jit_fault_call_second fault_call_second rbx=0xbbbbbbbbbbbbbbbb rsp=0
if jit_ok F3_jit_fault_call_second; then
    fault_common F3_jit_fault_call_second "$JIT_OUT" 0x140001005
    expect_hex F3_jit_fault_call_second "$JIT_OUT" rax 0x11
    expect_hex F3_jit_fault_call_second "$JIT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_jit_fault_call_second "$JIT_OUT" rsp 0x0
    expect_hex F3_jit_fault_call_second "$JIT_OUT" rip_out 0x140001005
fi
build_aot fault_call_second
aot_run F3_aot_fault_call_second fault_call_second rbx=0xbbbbbbbbbbbbbbbb rsp=0
if [ "$AOT_RC" -eq 0 ]; then
    expect_dec F3_aot_fault_call_second "$AOT_OUT" rc 1
    fault_common F3_aot_fault_call_second "$AOT_OUT" 0x140001005
    expect_hex F3_aot_fault_call_second "$AOT_OUT" rax 0x11
    expect_hex F3_aot_fault_call_second "$AOT_OUT" rbx 0xbbbbbbbbbbbbbbbb
    expect_hex F3_aot_fault_call_second "$AOT_OUT" rsp 0x0
    expect_hex F3_aot_fault_call_second "$AOT_OUT" rip_out 0x140001005
else
    fail "F3_aot_fault_call_second: probe failed rc=$AOT_RC"
fi

# ---------------------------------------------------------------------------
# F4: POP RSP and 16-bit push/pop
# ---------------------------------------------------------------------------
echo "== F4 POP RSP / 16-bit stack ==" | tee -a "$RESULTS/summary.txt"
jit_run F4_jit_pop_rsp pop_rsp rsp=mem+0x200 \
    mem=mem+0x200:8:mem+0x240 mem=mem+0x208:8:0xffffffffffffff00 \
    mem=mem+0x240:8:0xffffffffffffff00
if jit_ok F4_jit_pop_rsp; then
    base=$(hexfield mem_base "$JIT_OUT")
    rax_exp=$(printf '0x%x' $((base + 0x240)))
    rsp_exp=$(printf '0x%x' $((base + 0x248)))
    expect_hex F4_jit_pop_rsp "$JIT_OUT" rax "$rax_exp"
    expect_hex F4_jit_pop_rsp "$JIT_OUT" rsp "$rsp_exp"
    expect_dec F4_jit_pop_rsp "$JIT_OUT" error 0
fi
build_aot pop_rsp
aot_run F4_aot_pop_rsp pop_rsp rsp=mem+0x200 \
    mem=mem+0x200:8:mem+0x240 mem=mem+0x208:8:0xffffffffffffff00 \
    mem=mem+0x240:8:0xffffffffffffff00
if [ "$AOT_RC" -eq 0 ]; then
    base=$(hexfield mem_base "$AOT_OUT")
    rax_exp=$(printf '0x%x' $((base + 0x240)))
    rsp_exp=$(printf '0x%x' $((base + 0x248)))
    expect_dec F4_aot_pop_rsp "$AOT_OUT" rc 0
    expect_dec F4_aot_pop_rsp "$AOT_OUT" error 0
    expect_hex F4_aot_pop_rsp "$AOT_OUT" rax "$rax_exp"
    expect_hex F4_aot_pop_rsp "$AOT_OUT" rsp "$rsp_exp"
else
    fail "F4_aot_pop_rsp: probe failed rc=$AOT_RC"
fi
translate_fail push16 "unsupported 16-bit push/pop" "push ax"
jit_reject F4_push16 push16 "16-bit"
translate_fail pop16 "unsupported 16-bit push/pop" "pop ax"
jit_reject F4_pop16 pop16 "16-bit"

# ---------------------------------------------------------------------------
# F5: 64-bit IMUL CF/OF (and 32/16/8-bit regressions)
# ---------------------------------------------------------------------------
echo "== F5 IMUL flags ==" | tee -a "$RESULTS/summary.txt"
"$FLAGS_TEST" >"$LOGS/flags_host.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && ! grep -q '^FAIL ' "$LOGS/flags_host.out"; then
    pass "host aev_flags_imul cases"
else
    fail "host aev_flags_imul cases (rc=$rc)"
    grep '^FAIL ' "$LOGS/flags_host.out" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
fi
"$FLAGS_TEST" --dylib "$AOT_DYLIB" >"$LOGS/flags_aot.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && ! grep -q '^FAIL ' "$LOGS/flags_aot.out"; then
    pass "AOT-dylib aev_flags_imul cases"
else
    fail "AOT-dylib aev_flags_imul cases (rc=$rc)"
    grep '^FAIL ' "$LOGS/flags_aot.out" | sed 's/^/  /' | tee -a "$RESULTS/summary.txt"
fi

jit_flags() { # label object a b cf of
    local label=$1 obj=$2 a=$3 b=$4 cf=$5 of=$6
    jit_run "$label" "$obj" rax="$a" rbx="$b"
    if jit_ok "$label"; then
        expect_dec "$label" "$JIT_OUT" error 0
        expect_flags "$label" "$JIT_OUT" "$cf" "$of"
    fi
}
jit_flags F5_jit_w8_neg1_x_1 imul_flags_64 0xffffffffffffffff 1 0 0
jit_flags F5_jit_w8_min_x_1 imul_flags_64 0x8000000000000000 1 0 0
jit_flags F5_jit_w8_max_x_1 imul_flags_64 0x7fffffffffffffff 1 0 0
jit_flags F5_jit_w8_min_x_neg1 imul_flags_64 0x8000000000000000 0xffffffffffffffff 1 1
jit_flags F5_jit_w8_max_x_2 imul_flags_64 0x7fffffffffffffff 2 1 1
jit_flags F5_jit_w8_2p62_x_2 imul_flags_64 0x4000000000000000 2 1 1
jit_flags F5_jit_w8_neg2_x_max imul_flags_64 0xfffffffffffffffe 0x7fffffffffffffff 1 1
jit_flags F5_jit_w8_neg1_x_neg1 imul_flags_64 0xffffffffffffffff 0xffffffffffffffff 0 0
jit_flags F5_jit_w8_min_x_min imul_flags_64 0x8000000000000000 0x8000000000000000 1 1
jit_flags F5_jit_w4_neg1_x_1 imul_flags_32 0xffffffff 1 0 0
jit_flags F5_jit_w4_min_x_neg1 imul_flags_32 0x80000000 0xffffffff 1 1
jit_flags F5_jit_w4_max_x_2 imul_flags_32 0x7fffffff 2 1 1
jit_flags F5_jit_w4_2p30_x_2 imul_flags_32 0x40000000 2 1 1
jit_flags F5_jit_w4_neg1_x_neg1 imul_flags_32 0xffffffff 0xffffffff 0 0
jit_flags F5_jit_w2_neg1_x_1 imul_flags_16 0xffff 1 0 0
jit_flags F5_jit_w2_min_x_neg1 imul_flags_16 0x8000 0xffff 1 1
jit_flags F5_jit_w2_max_x_2 imul_flags_16 0x7fff 2 1 1
jit_flags F5_jit_w2_2p14_x_2 imul_flags_16 0x4000 2 1 1
jit_flags F5_jit_w2_neg1_x_neg1 imul_flags_16 0xffff 0xffff 0 0

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------
echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL SEMANTICS CHECKS PASSED"
    echo "results: $RESULTS"
    exit 0
else
    echo "$FAILURES SEMANTICS CHECK(S) FAILED"
    echo "results: $RESULTS"
    exit 1
fi
