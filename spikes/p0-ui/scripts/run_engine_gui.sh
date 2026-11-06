#!/usr/bin/env bash
# Run the engine-backed compat and native GUI paths plus a forced native
# failure (must exit non-zero with no compat fallback).
set -euo pipefail

BUILD_DIR=${1:?build dir}
OUT_ROOT=${2:?output root}
PYTHON=${PYTHON:-python3}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
LIB="$BUILD_DIR/libaev_bridge.dylib"
COFF="$ROOT/spikes/p0-engine/testdata/callback/aev_callback_fixture.obj"
AOT="${AEV_AOT_OVERRIDE:-$BUILD_DIR/generated/libcallback_aot.dylib}"
PROFILE="${AEV_COVERAGE_PROFILE:-base}"
MANIFEST="${AEV_SUPPLEMENT_MANIFEST:-}"
BASE_AOT="${AEV_BASE_AOT:-}"
FAILURES=0
mkdir -p "$OUT_ROOT"

promoted_rip_for_manifest() {
    "$PYTHON" - "$1" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
promoted = m.get("promoted_blocks") or []
print(hex(promoted[0]) if promoted else "")
PY
}

run_backend() {
    local backend=$1
    local out="$OUT_ROOT/$backend"
    rm -rf "$out"
    echo "== engine GUI backend=$backend =="
    if [ "$PROFILE" = "supplemented" ]; then
        if [ -z "$MANIFEST" ] || [ -z "$BASE_AOT" ]; then
            echo "FAIL: supplemented profile requires AEV_SUPPLEMENT_MANIFEST and AEV_BASE_AOT"
            FAILURES=$((FAILURES + 1))
            return 0
        fi
        local promoted
        promoted=$(promoted_rip_for_manifest "$MANIFEST")
        local promoted_opt=()
        if [ -n "$promoted" ]; then promoted_opt=(--promoted-rip "$promoted"); fi
        PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_engine \
            --backend "$backend" --lib "$LIB" --coff "$COFF" --aot "$AOT" \
            --base-aot "$BASE_AOT" \
            --coverage-profile supplemented --supplement-manifest "$MANIFEST" \
            "${promoted_opt[@]+"${promoted_opt[@]}"}" \
            --out "$out" >"$out.stdout" 2>"$out.stderr" || local rc=$?
        local rc=${rc:-0}
    else
        PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_engine \
            --backend "$backend" --lib "$LIB" --coff "$COFF" --aot "$AOT" \
            --coverage-profile base --promoted-rip 0x140001020 \
            --out "$out" >"$out.stdout" 2>"$out.stderr" || local rc=$?
        local rc=${rc:-0}
    fi
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
if not s.get("success") or len(s.get("checks") or {}) != 44:
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

echo "== supplemented profile rejects base artifact =="
if [ "$PROFILE" = "supplemented" ]; then
    NEG_OUT="$OUT_ROOT/supplemented-base-rejection"
    rm -rf "$NEG_OUT"
    set +e
    PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_engine \
        --backend native --lib "$LIB" --coff "$COFF" \
        --aot "$BUILD_DIR/generated/libcallback_aot.dylib" \
        --base-aot "$BASE_AOT" \
        --coverage-profile supplemented --supplement-manifest "$MANIFEST" \
        --out "$NEG_OUT" >"$NEG_OUT.stdout" 2>"$NEG_OUT.stderr"
    NEG_RC=$?
    set -e
    echo "supplemented-base rejection rc=$NEG_RC"
    if [ "$NEG_RC" -eq 0 ]; then
        echo "FAIL: supplemented profile accepted base artifact"
        FAILURES=$((FAILURES + 1))
    fi
    "$PYTHON" - "$NEG_OUT/summary.json" <<'PY'
import json, sys
s=json.load(open(sys.argv[1]))
if s.get("success") is not False or s.get("checks"):
    raise SystemExit("base artifact under supplemented profile did not fail before Guest")
if (s.get("backend_event_counts") or {}).get("compat", 0) != 0:
    raise SystemExit("compat fallback observed")
print("supplemented-base-negative", s.get("error"))
PY
else
    echo "skip (base profile)"
fi

echo "== supplemented profile rejects fake promoted provenance =="
if [ "$PROFILE" = "supplemented" ]; then
    FAKE_MAN="$OUT_ROOT/fake-promoted-manifest.json"
    "$PYTHON" - "$MANIFEST" "$FAKE_MAN" <<'PY'
import json, sys
m=json.load(open(sys.argv[1]))
base=m.get("base_selected_blocks") or []
if not base:
    raise SystemExit("fake-promoted fixture needs a non-empty base selected table")
m["promoted_blocks"]=[base[0]]
m["demoted_blocks"]=sorted(set(base)-{base[0]})
open(sys.argv[2],"w").write(json.dumps(m, indent=2)+"\n")
PY
    FAKE_OUT="$OUT_ROOT/fake-promoted-rejection"
    rm -rf "$FAKE_OUT"
    set +e
    PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_engine \
        --backend native --lib "$LIB" --coff "$COFF" --aot "$AOT" \
        --base-aot "$BASE_AOT" \
        --coverage-profile supplemented --supplement-manifest "$FAKE_MAN" \
        --out "$FAKE_OUT" >"$FAKE_OUT.stdout" 2>"$FAKE_OUT.stderr"
    FAKE_RC=$?
    set -e
    echo "fake-promoted rejection rc=$FAKE_RC"
    if [ "$FAKE_RC" -eq 0 ]; then
        echo "FAIL: supplemented profile accepted fake promoted provenance"
        FAILURES=$((FAILURES + 1))
    fi
    "$PYTHON" - "$FAKE_OUT/summary.json" <<'PY'
import json, sys
s=json.load(open(sys.argv[1]))
if s.get("success") is not False or s.get("checks"):
    raise SystemExit("fake promoted did not fail before Guest")
if (s.get("backend_event_counts") or {}).get("compat", 0) != 0:
    raise SystemExit("compat fallback observed")
print("fake-promoted-negative", s.get("error"))
PY
else
    echo "skip (base profile)"
fi

echo "== forced native failure =="
FAIL_OUT="$OUT_ROOT/native-failure"
rm -rf "$FAIL_OUT"
set +e
if [ "$PROFILE" = "supplemented" ]; then
    PROMOTED=$(promoted_rip_for_manifest "$MANIFEST")
    PROMOTED_OPT=()
    if [ -n "$PROMOTED" ]; then PROMOTED_OPT=(--promoted-rip "$PROMOTED"); fi
    PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_engine \
        --backend native --lib "$LIB" --coff "$COFF" --aot "$AOT" \
        --base-aot "$BASE_AOT" \
        --coverage-profile supplemented --supplement-manifest "$MANIFEST" \
        "${PROMOTED_OPT[@]+"${PROMOTED_OPT[@]}"}" \
        --out "$FAIL_OUT" --force-native-failure \
        >"$FAIL_OUT.stdout" 2>"$FAIL_OUT.stderr"
else
    PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_engine \
        --backend native --lib "$LIB" --coff "$COFF" --aot "$AOT" \
        --coverage-profile base --promoted-rip 0x140001020 \
        --out "$FAIL_OUT" --force-native-failure \
        >"$FAIL_OUT.stdout" 2>"$FAIL_OUT.stderr"
fi
FAIL_RC=$?
set -e
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
