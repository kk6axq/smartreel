#!/usr/bin/env python3
"""Print candidate SD-card mountpoints, one per line.

A candidate is a removable FAT/exFAT filesystem mounted under /media or
/run/media, excluding the Pico's "RPI-RP2" BOOTSEL drive. The deploy
script uses this to auto-locate the card; if it prints 0 or >1 lines the
user is asked to pass the path explicitly.
"""
import json
import subprocess

out = subprocess.run(
    ["lsblk", "-J", "-o", "NAME,FSTYPE,LABEL,MOUNTPOINT", "-e", "7"],  # -e7: skip loop devs
    capture_output=True, text=True).stdout

cands = []
def walk(dev):
    mp  = dev.get("mountpoint")
    fs  = dev.get("fstype")
    lbl = dev.get("label") or ""
    if (mp and fs in ("vfat", "exfat") and lbl != "RPI-RP2"
            and (mp.startswith("/media/") or mp.startswith("/run/media/"))):
        cands.append(mp)
    for child in dev.get("children", []) or []:
        walk(child)

try:
    for d in json.loads(out).get("blockdevices", []):
        walk(d)
except Exception:
    pass

for c in cands:
    print(c)
