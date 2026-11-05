#!/usr/bin/env bash
# Run the engine-backed compat and native GUI paths plus a forced native
# failure (must exit non-zero with no compat fallback).
set -u

BUILD_DIR=${1:?build dir}
OUT_ROOT=${2:?output root}
PYTHON=${PYTHON:-python3}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
LIB="$BUILD_DIR/libaev_bridge.dylib"
COFF="$ROOT/spikes/p0-engine/testdata/callback/aev_callback_fixture.obj"
AOT="$BUILD_DIR/generated/libcallback_aot.dylib"
FAILURES=0
mkdir -p "$OUT_ROOT"

run_backend() {
    local backend=$1
    local out="$OUT_ROOT/$backend"
    rm -rf "$out"
    echo "== engine GUI backend=$backend =="
    PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_engine \
        --backend "$backend" --lib "$LIB" --coff "$COFF" --aot "$AOT" \
        --out "$out" >"$out.stdout" 2>"$out.stderr"
    local rc=$?
    echo "$backend rc=$rc"
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: $backend exited $rc"
        FAILURES=$((FAILURES + 1))
    fi
    if [ -f "$out/summary.json" ]; then
        "$PYTHON" - "$out/summary.json" <<'PY'
import json, sys
s=json.load(open(sys.argv[1]))
print("success", s.get("success"), "checks", len(s.get("checks") or {}),
      "screenshots", len(s.get("screenshots") or []))
if not s.get("success"):
    raise SystemExit(1)
PY
        if [ $? -ne 0 ]; then
            echo "FAIL: $backend summary not successful"
            FAILURES=$((FAILURES + 1))
        fi
    else
        echo "FAIL: $backend summary missing"
        FAILURES=$((FAILURES + 1))
    fi
}

run_backend compat
run_backend native

echo "== cross-backend consistency =="
"$PYTHON" - "$OUT_ROOT/compat/summary.json" "$OUT_ROOT/native/summary.json" <<'PY'
import json, sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2]))
keys=("coff_sha256","aot_sha256","library_sha256")
bad=[]
for k in keys:
    if a.get(k)!=b.get(k): bad.append(k)
for state in ("sequence_a","sequence_b","sequence_c_hidden"):
    ra=(a.get("states",{}).get(state,{}).get("bridge") or {}).get("guest_rax")
    rb=(b.get("states",{}).get(state,{}).get("bridge") or {}).get("guest_rax")
    if ra!=rb: bad.append(f"{state}:{ra}!={rb}")
print("same_coff/aot/bridge", not bad, "bad", bad)
if bad: raise SystemExit(1)
PY
if [ $? -ne 0 ]; then
    echo "FAIL: compat/native did not use the same input/bridge/result"
    FAILURES=$((FAILURES + 1))
fi

echo "== forced native failure =="
FAIL_OUT="$OUT_ROOT/native-failure"
rm -rf "$FAIL_OUT"
PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_engine \
    --backend native --lib "$LIB" --coff "$COFF" --aot "$AOT" \
    --out "$FAIL_OUT" --force-native-failure \
    >"$FAIL_OUT.stdout" 2>"$FAIL_OUT.stderr"
FAIL_RC=$?
echo "forced native failure rc=$FAIL_RC"
if [ "$FAIL_RC" -eq 0 ]; then
    echo "FAIL: forced native failure unexpectedly exited 0"
    FAILURES=$((FAILURES + 1))
fi
"$PYTHON" - "$FAIL_OUT/summary.json" <<'PY'
import json, sys
s=json.load(open(sys.argv[1]))
counts=s.get("backend_event_counts") or {}
proof=s.get("backend_proof") or {}
print("forced_failure", s.get("forced_failure"),
      "compat_events", counts.get("compat"),
      "native_events", counts.get("native"),
      "compat_classes_used", proof.get("compat_classes_used"))
if not s.get("forced_failure") or counts.get("compat", 0) != 0:
    raise SystemExit(1)
if proof.get("compat_classes_used") is not False:
    raise SystemExit(1)
PY
if [ $? -ne 0 ]; then
    echo "FAIL: forced native failure showed compat fallback or missing evidence"
    FAILURES=$((FAILURES + 1))
fi

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL ENGINE GUI CHECKS PASSED"
    exit 0
fi
echo "$FAILURES ENGINE GUI CHECK(S) FAILED"
exit 1
