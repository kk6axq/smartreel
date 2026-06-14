#!/usr/bin/env python3
"""
SmartReel bench event injector.

Drives the RP2040 core test rig's USB console to inject fake reel events
onto the RS485 bus, and tails the ESP32 HMI's serial so you can watch the
round trip (inject -> RS485 -> HMI receives) in one place.

Ports are auto-detected by USB vendor ID, so it doesn't matter which
/dev/ttyACM* each board enumerates as:
    RP2040 (Pico)       VID 2e8a   -> "core"  (where events are injected)
    ESP32-S3 USB-JTAG   VID 303a   -> "hmi"   (the bus master, tailed)

Usage:
    debug-fw/inject.py                       # interactive console (+ HMI tail)
    debug-fw/inject.py insert 0 1650         # one-shot: send a command, report, exit
    debug-fw/inject.py --list                # show detected ports and exit
    debug-fw/inject.py --core /dev/ttyACM1   # override core port autodetect
    debug-fw/inject.py --hmi  /dev/ttyACM0   # override hmi port autodetect
    debug-fw/inject.py --no-hmi              # don't open/tail the HMI

Event commands (passed straight through to the core console; type 'help'
on the core for the full list):
    insert <reel> [mv]              REEL_INSERTED   (0x81)
    remove <reel>                   REEL_REMOVED    (0x82)
    press  <reel> <bit>             INPUT_CHANGE    (0x80, toggle one input)
    input  <reel> <newhex> [prev]   INPUT_CHANGE    (0x80, set 16-bit mask)
    sense  <reel> <mv> <thr>        SENSE_THRESHOLD (0x83)
    log    <level> <message...>     LOG             (0x8F)
    status | stats                  dump core state / counters

Interactive-only local commands (prefixed with '/'):
    /hmi     query + print the HMI's RS485 stats (tx/rx/events)
    /ports   show detected ports
    /help    this list
    /quit    exit
"""
import argparse
import re
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

VID_CORE = 0x2E8A   # Raspberry Pi (RP2040 / Pico)
VID_HMI  = 0x303A   # Espressif (ESP32-S3 USB-Serial-JTAG)

USE_COLOR = sys.stdout.isatty()
def _c(code, s):
    return f"\033[{code}m{s}\033[0m" if USE_COLOR else s
def core_tag():  return _c("36", "[core]")   # cyan
def hmi_tag():   return _c("32", "[hmi] ")   # green
def warn(s):     return _c("33", s)          # yellow

# HMI lines worth surfacing (skip boot/wifi spam); on_rs485_event() logs
# REEL_*/[core L..]/SENSE, and 'stats' prints the events= counter.
HMI_KEYWORDS = ("REEL_", "core L", "INPUT", "SENSE", "rs485", "events=")


def find_port(vid):
    for p in list_ports.comports():
        if p.vid == vid:
            return p.device
    return None


def detect_ports():
    return find_port(VID_CORE), find_port(VID_HMI)


class PortReader(threading.Thread):
    """Reads a serial port line-by-line and prints each via `emit`."""
    def __init__(self, ser, emit, line_filter=None):
        super().__init__(daemon=True)
        self.ser = ser
        self.emit = emit
        self.line_filter = line_filter
        self._stop = threading.Event()

    def run(self):
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(256)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").rstrip("\r")
                if not line:
                    continue
                if self.line_filter and not self.line_filter(line):
                    continue
                self.emit(line)

    def stop(self):
        self._stop.set()


def hmi_event_line(line):
    return any(k in line for k in HMI_KEYWORDS)


def read_hmi_events(hmi, timeout=0.8):
    """Synchronously query the HMI 'stats' and return the events counter."""
    if not hmi:
        return None
    try:
        hmi.reset_input_buffer()
        hmi.write(b"stats\n")
    except Exception:
        return None
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        buf += hmi.read(256)
        m = re.search(rb"events=(\d+)", buf)
        if m:
            return int(m.group(1))
    return None


def one_shot(core, hmi, command):
    """Send one command, then report the core echo + HMI events delta."""
    before = read_hmi_events(hmi)
    core.reset_input_buffer()
    core.write((command + "\n").encode())

    # collect core echo
    deadline = time.time() + 0.8
    cbuf = b""
    while time.time() < deadline:
        cbuf += core.read(256)
    for line in cbuf.decode(errors="replace").splitlines():
        if line.strip():
            print(f"{core_tag()} {line.strip()}")

    if hmi:
        # give the POLL cycle time to drain the event, then show HMI log
        deadline = time.time() + 1.2
        hbuf = b""
        while time.time() < deadline:
            hbuf += hmi.read(256)
        for line in hbuf.decode(errors="replace").splitlines():
            if line.strip() and hmi_event_line(line):
                print(f"{hmi_tag()} {line.strip()}")
        after = read_hmi_events(hmi)
        if before is not None and after is not None:
            delta = after - before
            note = "" if delta else warn("  (no new event seen -- check the link)")
            print(f"{hmi_tag()} events {before} -> {after}  (+{delta}){note}")


def interactive(core, hmi):
    print(f"core: {core_tag()}  hmi: {hmi_tag() if hmi else warn('(none)')}")
    print("Type an event command (insert/remove/press/input/sense/log), "
          "'help' for the core's list, or '/help' for local commands.")
    core_reader = PortReader(core, lambda l: print(f"\r{core_tag()} {l}"))
    core_reader.start()
    hmi_reader = None
    if hmi:
        hmi_reader = PortReader(hmi, lambda l: print(f"\r{hmi_tag()} {l}"),
                                line_filter=hmi_event_line)
        hmi_reader.start()

    try:
        while True:
            try:
                line = input()
            except EOFError:
                break
            line = line.strip()
            if not line:
                continue
            if line in ("/quit", "/q", "/exit"):
                break
            if line in ("/help", "/?"):
                print(__doc__.split("Interactive-only")[1])
                continue
            if line == "/ports":
                c, h = detect_ports()
                print(f"  core (2e8a): {c}\n  hmi  (303a): {h}")
                continue
            if line == "/hmi":
                ev = read_hmi_events(hmi)
                print(f"{hmi_tag()} events={ev}" if ev is not None
                      else warn("no HMI / no response"))
                continue
            # forward to the core console
            core.write((line + "\n").encode())
            # nudge the HMI to report its events counter so INPUT_CHANGE
            # (which the HMI doesn't log) is still visible as a round trip
            if hmi and line.split()[0] in ("insert", "remove", "press",
                                           "input", "sense", "log"):
                time.sleep(0.4)
                hmi.write(b"stats\n")
    except KeyboardInterrupt:
        pass
    finally:
        core_reader.stop()
        if hmi_reader:
            hmi_reader.stop()
        print("\nbye.")


def main():
    ap = argparse.ArgumentParser(
        description="Inject fake SmartReel events via the RP2040 core console.",
        epilog="Run with no command for an interactive console.")
    ap.add_argument("command", nargs="*", help="one-shot core command, e.g. insert 0 1650")
    ap.add_argument("--core", help="core serial port (default: autodetect VID 2e8a)")
    ap.add_argument("--hmi", help="hmi serial port (default: autodetect VID 303a)")
    ap.add_argument("--no-hmi", action="store_true", help="don't open/tail the HMI")
    ap.add_argument("--list", action="store_true", help="show detected ports and exit")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    auto_core, auto_hmi = detect_ports()
    if args.list:
        print(f"core (RP2040, VID 2e8a): {auto_core or warn('not found')}")
        print(f"hmi  (ESP32,  VID 303a): {auto_hmi or warn('not found')}")
        print("\nall serial ports:")
        for p in list_ports.comports():
            vid = f"{p.vid:04x}" if p.vid else "----"
            print(f"  {p.device}  vid={vid}  {p.description}")
        return

    core_port = args.core or auto_core
    if not core_port:
        sys.exit(warn("RP2040 core not found (VID 2e8a). Plug it in or pass --core. "
                      "Use --list to see ports."))
    hmi_port = None if args.no_hmi else (args.hmi or auto_hmi)

    core = serial.Serial(core_port, args.baud, timeout=0.2)
    hmi = serial.Serial(hmi_port, args.baud, timeout=0.2) if hmi_port else None
    time.sleep(0.3)

    try:
        if args.command:
            one_shot(core, hmi, " ".join(args.command))
        else:
            interactive(core, hmi)
    finally:
        core.close()
        if hmi:
            hmi.close()


if __name__ == "__main__":
    main()
