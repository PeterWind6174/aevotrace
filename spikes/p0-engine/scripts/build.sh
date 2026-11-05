#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD_DIR=${1:-"$ROOT/.local/build-p0-engine"}
RESULTS_DIR=${AEV_RESULTS_DIR:-"$ROOT/.local/results/P0-ENGINE-01/final"}

cmake -S "$ROOT/spikes/p0-engine" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD_DIR"
AEV_RESULTS_DIR="$RESULTS_DIR" \
    ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "build: $BUILD_DIR"
echo "results: $RESULTS_DIR"
