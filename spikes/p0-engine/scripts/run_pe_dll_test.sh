#!/usr/bin/env bash
# P0-PE-DLL-01: real PE32+ DLL through the shared AOT/JIT core.
set -euo pipefail
BUILD_DIR=${1:?build dir}
SRC_DIR=${2:?source dir}
OUT=${AEV_RESULTS_DIR:-"$BUILD_DIR/results-pe-dll"}
mkdir -p "$OUT"
PYTHON=${PYTHON:-python3}
"$PYTHON" "$SRC_DIR/scripts/pe_dll_test.py" \
    --build "$BUILD_DIR" --source "$SRC_DIR" --results "$OUT"
