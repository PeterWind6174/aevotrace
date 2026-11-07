#!/usr/bin/env bash
# P0-PE-GUI-01: run the persistent PE-session GUI for compat and native.
#
# Usage:
#   scripts/run_pe_gui_test.sh <build-dir> <output-root>
#
# Required build output:
#   <build-dir>/libaev_pe_session.dylib
#   <build-dir>/generated/libpe_aot.dylib  (or set AEV_PE_AOT)
#
# Optional environment:
#   AEV_PE_SESSION_LIB, AEV_PE_DLL, AEV_PE_REGISTRY, AEV_PE_AOT,
#   AEV_COVERAGE_PROFILE=base|supplemented,
#   AEV_PE_BASE_AOT, AEV_PE_SUPPLEMENT_MANIFEST,
#   AEV_PE_TRACE=0|1, AEV_PE_LOAD_BASE, PYTHON
#
# This script fails with exit 2 when the session library is not built yet; it
# never fabricates a GUI result or silently skips the PE path.
#
# Supplied profile: run the base profile first and feed each backend's
# trace.jsonl to the compiler-owned aot_supplement.py, then re-run with
# AEV_COVERAGE_PROFILE=supplemented.  The supplement must be built from a
# trace that covers the same GUI export sequence (Reset/Compute/State/Calls)
# or the zero-JIT check below is expected to fail.
set -euo pipefail

BUILD_DIR=${1:?build dir}
OUT_ROOT=${2:?output root}
PYTHON=${PYTHON:-python3}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
LIB=${AEV_PE_SESSION_LIB:-"$BUILD_DIR/libaev_pe_session.dylib"}
DLL=${AEV_PE_DLL:-"$ROOT/spikes/p0-engine/testdata/pe/aevguest.dll"}
REGISTRY=${AEV_PE_REGISTRY:-"$ROOT/spikes/p0-engine/testdata/pe/aevimports.txt"}
PROFILE=${AEV_COVERAGE_PROFILE:-base}
LOAD_BASE=${AEV_PE_LOAD_BASE:-0x180000000}
TRACE=${AEV_PE_TRACE:-1}
BASE_AOT=${AEV_PE_BASE_AOT:-}
MANIFEST=${AEV_PE_SUPPLEMENT_MANIFEST:-}
FAILURES=0
mkdir -p "$OUT_ROOT"

if [ -z "${AEV_PE_AOT:-}" ]; then
    for candidate in "$BUILD_DIR/generated/libpe_aot.dylib" \
                     "$BUILD_DIR/libpe_aot.dylib"; do
        if [ -f "$candidate" ]; then
            AOT="$candidate"
            break
        fi
    done
else
    AOT="$AEV_PE_AOT"
fi
AOT=${AOT:-"$BUILD_DIR/generated/libpe_aot.dylib"}

if [ ! -f "$LIB" ]; then
    echo "PENDING: PE session library not found: $LIB"
    echo "Build the compiler-owned libaev_pe_session.dylib first; refusing to fake a GUI result."
    exit 2
fi
if [ ! -f "$AOT" ]; then
    echo "FAIL: PE AOT dylib not found: $AOT (set AEV_PE_AOT or AEV_PE_AOT)"
    exit 2
fi

run_backend() {
    local backend=$1
    local out="$OUT_ROOT/$backend"
    rm -rf "$out"
    mkdir -p "$out"
    local args=(--backend "$backend" --lib "$LIB" --dll "$DLL"
                --registry "$REGISTRY" --aot "$AOT" --load-base "$LOAD_BASE"
                --out "$out")
    if [ "$PROFILE" = "supplemented" ]; then
        if [ -z "$BASE_AOT" ] || [ -z "$MANIFEST" ]; then
            echo "FAIL: supplemented profile requires AEV_PE_BASE_AOT and AEV_PE_SUPPLEMENT_MANIFEST"
            return 1
        fi
        args+=(--coverage-profile supplemented --base-aot "$BASE_AOT"
               --supplement-manifest "$MANIFEST")
    fi
    if [ "$TRACE" = "1" ]; then
        args+=(--trace "$out/trace.jsonl")
    fi
    echo "== PE GUI backend=$backend profile=$PROFILE =="
    local rc=0
    PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_pe_gui \
        "${args[@]}" >"$out.stdout" 2>"$out.stderr" || rc=$?
    echo "$backend rc=$rc"
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: $backend exited $rc (see $out.stdout / $out.stderr)"
        FAILURES=$((FAILURES + 1))
    fi
    if [ ! -f "$out/summary.json" ]; then
        echo "FAIL: $backend summary.json missing"
        FAILURES=$((FAILURES + 1))
        return 1
    fi
    if ! "$PYTHON" - "$out/summary.json" "$backend" "$PROFILE" "$TRACE" <<'PY'
import json, sys
path, backend, profile, trace_enabled = sys.argv[1:5]
s = json.load(open(path))
errors = []
if not s.get("success"):
    errors.append("success is false")
proof = s.get("session_proof") or {}
counts = proof.get("counts") or {}
if counts.get("load_count") != 1:
    errors.append(f"load_count={counts.get('load_count')}")
if proof.get("session_create_count") != 1:
    errors.append(f"session_create_count={proof.get('session_create_count')}")
if len(s.get("screenshots") or []) < 4:
    errors.append(f"screenshots={len(s.get('screenshots') or [])}")
if not (s.get("extra_checks") or {}).get("no_unicorn"):
    errors.append("unicorn loaded")
if not (s.get("extra_checks") or {}).get("no_pefile"):
    errors.append("pefile loaded")
if not (s.get("extra_checks") or {}).get("no_guest_pe"):
    errors.append("guest_pe loaded")
if profile == "base":
    if counts.get("jit_compiles_total", 0) <= 0:
        errors.append("base profile has no JIT compile")
    if counts.get("jit_execs_total", 0) <= 0:
        errors.append("base profile has no JIT exec")
else:
    if counts.get("jit_compiles_total", 0) != 0:
        errors.append("supplemented profile compiled JIT")
    if counts.get("jit_execs_total", 0) != 0:
        errors.append("supplemented profile executed JIT")
    if counts.get("aot_blocks_total", 0) <= 0:
        errors.append("supplemented profile has no AOT block")
tp = s.get("trace_proof")
if trace_enabled == "1" and not tp:
    errors.append("trace proof missing")
if tp:
    if int(tp["header"].get("trace_abi", 0)) != 4:
        errors.append("trace ABI is not 4")
    if profile == "supplemented" and tp.get("jit_compile_events"):
        errors.append("supplemented trace has jit_compile")
    if profile == "supplemented" and tp.get("jit_exec_events"):
        errors.append("supplemented trace has jit_exec")
print(f"{backend}: success={s.get('success')} checks={s.get('check_count')} "
      f"screenshots={len(s.get('screenshots') or [])} "
      f"load_count={counts.get('load_count')} calls_total={counts.get('calls_total')} "
      f"host_imports={counts.get('host_import_calls')} "
      f"aot={counts.get('aot_blocks_total')} jit={counts.get('jit_execs_total')}")
if errors:
    print("FAIL:", "; ".join(errors))
    raise SystemExit(1)
PY
    then
        FAILURES=$((FAILURES + 1))
    fi
}

run_backend compat
run_backend native

echo "== cross-backend consistency =="
if ! "$PYTHON" - "$OUT_ROOT/compat/summary.json" "$OUT_ROOT/native/summary.json" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))
bad = []
for key in ("dll_sha256", "registry_sha256", "aot_sha256",
            "library_sha256", "load_base", "coverage_profile"):
    if a.get(key) != b.get(key):
        bad.append(f"{key}: {a.get(key)} != {b.get(key)}")
for state in ("compute_prep", "compute_hidden", "compute_empty",
              "compute_independent", "recovery"):
    sa = (a.get("states") or {}).get(state) or {}
    sb = (b.get("states") or {}).get(state) or {}
    for key in ("result_text", "state_text", "calls_text"):
        if sa.get(key) != sb.get(key):
            bad.append(f"{state}.{key}: {sa.get(key)} != {sb.get(key)}")
pa = a.get("session_proof") or {}
pb = b.get("session_proof") or {}
if (pa.get("info") or {}).get("file_sha256") != (pb.get("info") or {}).get("file_sha256"):
    bad.append("session file SHA differs")
if (pa.get("info") or {}).get("aot_sha256") != (pb.get("info") or {}).get("aot_sha256"):
    bad.append("session AOT SHA differs")
if (pa.get("counts") or {}).get("load_count") != 1:
    bad.append("compat load_count != 1")
if (pb.get("counts") or {}).get("load_count") != 1:
    bad.append("native load_count != 1")
print("same DLL/AOT/library and business state:", not bad)
if bad:
    for item in bad:
        print("FAIL:", item)
    raise SystemExit(1)
PY
then
    FAILURES=$((FAILURES + 1))
fi

echo "== forced native failure (no compat fallback) =="
FAIL_OUT="$OUT_ROOT/native-forced-failure"
rm -rf "$FAIL_OUT"
set +e
PYTHONPATH="$ROOT/spikes/p0-ui" "$PYTHON" -m harness.run_pe_gui \
    --backend native --lib "$LIB" --dll "$DLL" --registry "$REGISTRY" \
    --aot "$AOT" --load-base "$LOAD_BASE" --out "$FAIL_OUT" \
    --force-native-failure >"$FAIL_OUT.stdout" 2>"$FAIL_OUT.stderr"
FAIL_RC=$?
set -e
echo "forced native failure rc=$FAIL_RC"
if [ "$FAIL_RC" -eq 0 ]; then
    echo "FAIL: forced native failure unexpectedly exited 0"
    FAILURES=$((FAILURES + 1))
fi
if [ ! -f "$FAIL_OUT/summary.json" ]; then
    echo "FAIL: forced native failure summary missing"
    FAILURES=$((FAILURES + 1))
else
    if ! "$PYTHON" - "$FAIL_OUT/summary.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
counts = s.get("backend_event_counts") or {}
proof = s.get("backend_proof") or {}
print("forced_failure", s.get("forced_failure"),
      "compat_events", counts.get("compat"),
      "native_events", counts.get("native"),
      "compat_classes_used", proof.get("compat_classes_used"))
if not s.get("forced_failure"):
    raise SystemExit("forced failure not marked")
if counts.get("compat", 0) != 0:
    raise SystemExit("compat fallback observed")
if proof.get("compat_classes_used") is not False:
    raise SystemExit("compat classes used by native fallback")
if (s.get("process") or {}).get("runner_process_spawned") is not False:
    raise SystemExit("runner process evidence missing")
PY
    then
        echo "FAIL: forced native failure evidence invalid"
        FAILURES=$((FAILURES + 1))
    fi
fi

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL PE GUI CHECKS PASSED"
    exit 0
fi
echo "$FAILURES PE GUI CHECK(S) FAILED"
exit 1
