#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
exec ./scripts/run_backend.sh native "${1:-artifacts/p0_gui_test.dll}" "${2:-evidence/native}"
