#!/usr/bin/env python3
"""
Board control helper for the flash scripts. Finds boards by USB vendor ID
(so it's immune to /dev/ttyACM* swaps) and pokes them into bootloader mode.

  _ctl.py find  <core|hmi>   # print the device path (empty if not present)
  _ctl.py dl    <hmi>        # send 'dl' to reboot an ESP32 into download mode
  _ctl.py touch <core>       # 1200-baud touch to reboot an RP2040 into BOOTSEL
"""
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

VID = {"core": 0x2E8A, "hmi": 0x303A}   # RP2040 / ESP32-S3 USB-JTAG


def find(role):
    for p in list_ports.comports():
        if p.vid == VID[role]:
            return p.device
    return ""


def main():
    if len(sys.argv) < 3 or sys.argv[2] not in VID:
        sys.exit(__doc__)
    action, role = sys.argv[1], sys.argv[2]
    dev = find(role)

    if action == "find":
        print(dev)
        return
    if not dev:
        return  # nothing to poke; caller handles the "already in bootloader" case
    if action == "touch":          # RP2040 -> BOOTSEL
        try:
            serial.Serial(dev, 1200)
            time.sleep(0.2)
        except Exception:
            pass
    elif action == "dl":           # ESP32 app -> ROM download mode
        try:
            s = serial.Serial(dev, 115200, timeout=0.5)
            time.sleep(0.3)
            s.write(b"dl\n")
            time.sleep(0.5)
            s.close()
        except Exception:
            pass
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
