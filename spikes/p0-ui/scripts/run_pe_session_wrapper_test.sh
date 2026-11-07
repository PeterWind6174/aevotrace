#!/usr/bin/env bash
# P0-PE-SESSION-FIX-01/F1 wrapper test entry point for ctest.
#
# Usage:
#   run_pe_session_wrapper_test.sh [build-dir] [results-dir]
#
# Environment:
#   PYTHON   interpreter to use (default: python3; no venv/PyObjC required)
#
# The build dir must contain libaev_pe_session.dylib.  The source tree and
# testdata are derived from this script's location.  Exit code is the Python
# test's exit code (0 pass, 1 regression, 2 missing inputs).
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SOURCE=$(cd "$SCRIPT_DIR/../../.." && pwd)
BUILD=${1:-"$SOURCE/.local/build-pe-session-fix-platform"}
RESULTS=${2:-"$BUILD/wrapper-observer-results"}
PYTHON=${PYTHON:-python3}

exec "$PYTHON" "$SCRIPT_DIR/pe_session_wrapper_test.py" \
    --build "$BUILD" --source "$SOURCE" --results "$RESULTS"
