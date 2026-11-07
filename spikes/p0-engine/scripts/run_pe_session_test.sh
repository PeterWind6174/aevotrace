#!/usr/bin/env bash
# P0-PE-GUI-01: versioned persistent PE session C ABI headless test.
set -euo pipefail
BUILD_DIR=${1:?build dir}
SRC_DIR=${2:?source dir}
OUT=${AEV_RESULTS_DIR:-"$BUILD_DIR/results-pe-session"}
mkdir -p "$OUT"
PYTHON=${PYTHON:-python3}
"$PYTHON" "$SRC_DIR/scripts/pe_session_test.py" \
    --build "$BUILD_DIR" --source "$SRC_DIR" --results "$OUT"
