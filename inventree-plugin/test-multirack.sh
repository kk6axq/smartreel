#!/usr/bin/env bash
# Run the multi-rack live test against the local InvenTree docker instance.
set -euo pipefail
cd "$(dirname "$0")"
exec python3 test_multirack.py "$@"
