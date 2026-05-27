#!/usr/bin/env bash
# Build + flash the ESP32 HMI firmware (esp32-hmi) over USB-JTAG.
set -euo pipefail
source "$(dirname "$0")/lib.sh"
flash_esp32 esp32-hmi
