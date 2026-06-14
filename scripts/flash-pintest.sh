#!/usr/bin/env bash
# Build + flash the standalone ESP32 RS485 pin-toggle tool (esp32-rs485-pintest).
set -euo pipefail
source "$(dirname "$0")/lib.sh"
flash_esp32 debug-fw/esp32-rs485-pintest
