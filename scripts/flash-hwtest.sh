#!/usr/bin/env bash
# Build + flash the RP2040 hardware bring-up test (core-hwtest) over UF2.
set -euo pipefail
source "$(dirname "$0")/lib.sh"
flash_rp2040 debug-fw/core-hwtest
