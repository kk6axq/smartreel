#!/usr/bin/env bash
# Build + flash the RP2040 core test rig (core-fw) over UF2.
set -euo pipefail
source "$(dirname "$0")/lib.sh"
flash_rp2040 core-fw
