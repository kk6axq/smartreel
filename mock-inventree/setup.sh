#!/usr/bin/env bash
# One-off venv + deps install + TLS cert. Re-running is idempotent.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

if [ ! -d .venv ]; then
    python3 -m venv .venv
fi
./.venv/bin/pip install --upgrade pip >/dev/null
./.venv/bin/pip install -r requirements.txt

./gen-cert.sh

echo "Done. Run with ./run.sh"
