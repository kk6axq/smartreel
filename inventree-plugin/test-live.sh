#!/usr/bin/env bash
# Run the live contract test against the local InvenTree docker instance.
# (Needs the stack at ~/Desktop/Projects/inventree running.)
set -euo pipefail
cd "$(dirname "$0")"
exec python3 test_live.py "$@"
