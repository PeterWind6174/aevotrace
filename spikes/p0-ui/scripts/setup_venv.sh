#!/bin/sh
# P0-only environment. The venv and wheels are not committed.
set -eu
cd "$(dirname "$0")/.."
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --only-binary=:all: -r requirements.txt
python - <<'PY'
import unicorn, pefile, objc
print("unicorn", unicorn.__version__)
print("pefile", pefile.__version__)
print("pyobjc", objc.__version__)
PY
