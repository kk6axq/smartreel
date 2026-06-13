# Shared helpers for the SmartReel flash scripts. Source this; don't run it.
#
#   flash_rp2040 <project-dir>   build + flash an RP2040 project over UF2
#   flash_esp32  <project-dir>   build + flash an ESP32 project over USB-JTAG
#
# Boards are located by USB VID via scripts/_ctl.py, so it doesn't matter
# which /dev/ttyACM* each enumerates as.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CTL="python3 $REPO/scripts/_ctl.py"

die() { echo "error: $*" >&2; exit 1; }

# --- RP2040: build, 1200-baud touch to BOOTSEL, copy the UF2 -----------------
flash_rp2040() {
    local dir="$REPO/$1"
    echo "== build $1 =="
    ( cd "$dir" && pio run ) || die "build failed"

    # If a serial port is present the board is running firmware: touch it to
    # BOOTSEL. If not, assume it's already in BOOTSEL.
    if [ -n "$($CTL find core)" ]; then
        echo "== reboot RP2040 to BOOTSEL =="
        $CTL touch core
    fi

    echo "== wait for RPI-RP2 drive =="
    local mnt=""
    for _ in $(seq 1 40); do
        mnt="$(lsblk -o LABEL,MOUNTPOINT -nr 2>/dev/null | awk '$1=="RPI-RP2"{print $2; exit}')"
        [ -n "$mnt" ] && break
        sleep 0.5
    done
    [ -n "$mnt" ] || die "RPI-RP2 drive not found (is the board in BOOTSEL?)"

    local uf2="$dir/.pio/build/pico/firmware.uf2"
    [ -f "$uf2" ] || die "UF2 not found: $uf2"
    echo "== copy $uf2 -> $mnt =="
    cp "$uf2" "$mnt"/ && sync
    echo "done: $1 flashed (RP2040 reboots into it automatically)"
}

# --- ESP32: build, drop into download mode, esptool upload (with retry) ------
flash_esp32() {
    local dir="$REPO/$1"

    # If the app is running it has a 'dl' command; use it to enter ROM
    # download mode. If it's already in download mode this is a no-op.
    if [ -n "$($CTL find hmi)" ]; then
        echo "== reboot ESP32 to download mode =="
        $CTL dl hmi
        sleep 6   # let the USB-JTAG re-enumerate and settle
    fi

    local port
    port="$($CTL find hmi)"
    [ -n "$port" ] || die "ESP32 (VID 303a) not found"

    # Build + upload in a SINGLE pio invocation. Running `pio run` and then
    # a separate `pio run -t upload` pays PlatformIO's ~15 s LDF/registry-
    # lookup overhead twice (the sources compile once, but every pio run
    # re-scans deps), so we fold the build into the upload target. The
    # USB-JTAG sometimes returns a write-timeout on the first connect right
    # after re-enumeration; retry a couple of times -- the firmware is
    # already built by then, so retries don't recompile.
    echo "== build + upload $1 =="
    local ok=0
    for attempt in 1 2 3; do
        echo "== upload attempt $attempt -> $port =="
        if ( cd "$dir" && pio run -t upload --upload-port "$port" ); then
            ok=1; break
        fi
        echo "   upload failed; settling and retrying..."
        sleep 4
        port="$($CTL find hmi)"   # re-detect in case it moved
    done
    [ "$ok" = 1 ] || die "upload failed after retries"
    echo "done: $1 flashed. NOTE: press RESET on the ESP32 to boot the app"
    echo "      (it stays in download mode after flashing, by design)."
}
