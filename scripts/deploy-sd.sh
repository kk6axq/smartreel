#!/usr/bin/env bash
# Build both firmware images and copy them onto the SD card as the update
# files the HMI looks for:  hmi.bin  (ESP32) and  core.bin  (RP2040).
#
#   scripts/deploy-sd.sh [SD_MOUNTPOINT]
#
# With no argument the SD is auto-detected (a removable FAT volume, not
# the Pico's RPI-RP2 BOOTSEL drive). Insert the card into a PC card
# reader first; afterwards move it back to the ESP32.
set -euo pipefail
source "$(dirname "$0")/lib.sh"   # REPO, die

SD="${1:-}"
if [ -z "$SD" ]; then
    mapfile -t cands < <(python3 "$REPO/scripts/_find_sd.py")
    if   [ "${#cands[@]}" -eq 0 ]; then
        die "no SD card mount found -- insert the card (PC reader) or pass the path: deploy-sd.sh /media/you/CARD"
    elif [ "${#cands[@]}" -gt 1 ]; then
        printf 'multiple removable FAT mounts found:\n'
        printf '  %s\n' "${cands[@]}"
        die "pass the SD path explicitly: deploy-sd.sh <mountpoint>"
    fi
    SD="${cands[0]}"
fi
[ -d "$SD" ] || die "not a directory: $SD"
[ -w "$SD" ] || die "not writable: $SD"
echo "SD target: $SD"

# build <project-dir>, copy <built-bin> to <SD>/<dest>, report version
build_and_copy() {
    local dir="$REPO/$1" bin="$1/$2" dest="$3"
    echo "== build $1 =="
    ( cd "$dir" && pio run ) >/dev/null || die "build failed: $1"
    cp "$REPO/$bin" "$SD/$dest"
    python3 - "$REPO/$bin" "$dest" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
i = d.find(b"SRFWTAG1")
if i < 0:
    print(f"  -> {sys.argv[2]}  (no version tag, {len(d)} B)")
else:
    proj = d[i+12:i+20].split(b"\x00")[0].decode("ascii", "replace")
    print(f"  -> {sys.argv[2]}  v{d[i+8]}.{d[i+9]}.{d[i+10]} [{proj}]  ({len(d)} B)")
PY
}

build_and_copy esp32-hmi ".pio/build/waveshare_esp32s3_43b/firmware.bin" "hmi.bin"
build_and_copy core-fw   ".pio/build/pico/firmware.bin"                  "core.bin"
sync
echo "done. Eject the card and move it back to the ESP32."
