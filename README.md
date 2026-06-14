# SmartReel

SmartReel is a smart storage rack for reels of surface-mount electronic
components that works hand-in-hand with [InvenTree](https://inventree.org/),
an open-source inventory management system. The rack is built from one or more
**reel modules**, each a row of slots; every slot senses whether a reel is
present, has an addressable RGB LED to guide the operator, and can be widened by
pulling **dividers**. A touchscreen **HMI** mounted on the rack is the operator's
control panel — it talks to the rack hardware over RS485 and to InvenTree over
HTTPS. InvenTree stays the single source of truth for inventory; SmartReel
records *where* each reel physically lives and lights slots to guide pick and
put-away. The operator scans a reel, the rack lights up, and the inventory
updates itself.

## Repository layout

```
esp32-hmi/         ESP32-S3 touchscreen HMI firmware (C++ / PlatformIO / LVGL)
core-fw/           RP2040 core firmware that drives the reel modules (C++ / PlatformIO)
inventree-plugin/  InvenTree server-side plugin (Python)
pcbs/              KiCad hardware designs
  CorePCB/           Core PCB
  ReelPCB/           reel module PCB
debug-fw/          Bench/debug tooling (not shipped)
  mock-inventree/    FastAPI stand-in for the plugin, used during HMI development
  core-hwtest/       RP2040 hardware bring-up test
  esp32-rs485-pintest/ standalone ESP32 RS485 pin-toggle tool
  inject.py          bench event injector (drives the core test rig over USB)
  sd-assets/         mock QR codes for bench-testing the scan->resolve flow
docs/              Project documentation (start at docs/README.md)
labels/            Printable slot label PDFs (make_labels.py; PDFs git-ignored)
scripts/           Flashing, SD deployment, QR generation helpers
```

## Documentation

The full documentation set lives under [`docs/`](docs/README.md). Start with the
[documentation index](docs/README.md), then dive into the manual chapters:

- [Concept of Operations](docs/manual/01-concept-of-operations.md) — what SmartReel is and the end-to-end workflows.
- [Hardware Structure](docs/manual/02-hardware.md) — rack, Core PCB, reel modules, LEDs, sensors, wiring.
- [Software Structure](docs/manual/03-software-architecture.md) — components, subsystems, and system data flow.
- [API Reference](docs/manual/04-api-reference.md) — the HMI↔plugin HTTP API and the RS485 wire protocol.
- [InvenTree Integration](docs/manual/05-inventree-integration.md) — the plugin, install/config, and the sync model.
- [Operator User Manual](docs/manual/06-operator-manual.md) — task-oriented guide for shop-floor operators.

## Build & flash

**HMI firmware** (`esp32-hmi/`) and **Core firmware** (`core-fw/`) are
[PlatformIO](https://platformio.org/) projects. Build and flash:

```bash
# HMI (ESP32-S3) — requires manual boot mode (hold BOOT, tap RESET, release BOOT)
cd esp32-hmi && pio run
./scripts/flash-hmi.sh

# Core (RP2040) — UF2 dropped onto the BOOTSEL drive
cd core-fw && pio run
./scripts/flash-core.sh
```

The flash scripts locate the boards by USB VID, so the order they are plugged in
does not matter. See [`scripts/`](scripts/) for SD deployment (`deploy-sd.sh`)
and QR generation helpers.

**InvenTree plugin** (`inventree-plugin/`) is a Python plugin installed into an
InvenTree instance. See [`inventree-plugin/README.md`](inventree-plugin/README.md)
for installation, configuration, and the docker dev workflow.

**Mock server** — for HMI development without a live InvenTree, the FastAPI mock
at [`debug-fw/mock-inventree/`](debug-fw/mock-inventree/) serves the same HTTPS
contract as the plugin. See its [README](debug-fw/mock-inventree/README.md).
