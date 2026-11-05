#!/bin/sh
# Run one backend against the guest DLL.
#   ./scripts/run_backend.sh compat|native path/to/p0_gui_test.dll evidence/<name>
set -eu
cd "$(dirname "$0")/.."
backend=${1:?backend compat|native}
dll=${2:?path to p0_gui_test.dll}
out=${3:-evidence/$backend}
. .venv/bin/activate
exec python3 -m harness.run --backend "$backend" --dll "$dll" --out "$out"
