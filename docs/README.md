# SmartReel Documentation

SmartReel is a smart electronics-component-reel storage rack that integrates
with [InvenTree](https://inventree.org/) for inventory management. An operator
stores reels of SMT components in physical slots; the rack tracks what is in
each slot, lights slots up to guide picking and put-away, and keeps inventory
in sync with InvenTree.

This directory is the documentation set for the whole project. It is written
against what the code actually does as of June 2026; where older design notes
disagree with the implementation, the implementation wins and the discrepancy
is called out.

## Documentation set (`docs/manual/`)

| Doc | What it covers |
|---|---|
| [Concept of Operations](manual/01-concept-of-operations.md) | What SmartReel is, who uses it, and the end-to-end workflows (commission, stock, pick, put-away, reconcile, firmware update). |
| [Hardware Structure](manual/02-hardware.md) | The rack, Core PCB, reel modules, ports/chaining, LEDs, sensors, HMI display, power, and RS485 wiring. |
| [Software Structure](manual/03-software-architecture.md) | Each software component, its subsystems and responsibilities, and a system-level data-flow diagram. |
| [API Reference](manual/04-api-reference.md) | The HMI↔plugin HTTP API (endpoints, auth, idempotency, rack identity) and the RS485 wire protocol (framing, message types, FW update). |
| [InvenTree Integration](manual/05-inventree-integration.md) | How the plugin works, installation/config, the op-queue + reconcile sync model, online/offline behaviour, and multi-rack. |
| [Operator User Manual](manual/06-operator-manual.md) | Plain-language, task-oriented guide for shop-floor operators using the HMI touchscreen. |

## Reference material (this directory)

These are the working documents the manual was built from. They remain useful
for detail and history but are not the authoritative manual:

- `hmi-plugin-api.md` — original API contract (superseded in detail by
  `manual/04-api-reference.md`, but still the canonical short-form contract).
- `smartreel-rs485-protocol.md` — original RS485 design note. Note its message
  catalog lists firmware messages under category `0xF_`; the **code implements
  them at `0x30`–`0x35`** (see API reference for why).
- `user-stories.md` — the behavioural source of truth for the workflows.
- `roadmap.md`, `session-handoff.md`, `session-notes-*.md` — project history.
- `known-bugs.md` — open and fixed issues.
- `test-plan.md` — verification plan.

## Component layout (repository root)

```
esp32-hmi/         ESP32-S3 touchscreen HMI firmware (C++ / PlatformIO / LVGL)
core-fw/           RP2040 core firmware that drives the reel modules (C++ / PlatformIO)
inventree-plugin/  InvenTree server-side plugin (Python)
mock-inventree/    FastAPI stand-in for the plugin, used during HMI development
CorePCB/           KiCad design for the Core PCB
ReelPCB/           KiCad design for the reel module PCB
labels/            Printable slot label PDFs
scripts/, tools/   Flashing, SD deployment, QR/label generation helpers
sd-assets/         Files staged onto the HMI's SD card (offline catalog, etc.)
```

> **In-progress areas.** Two features were being developed alongside this
> documentation: an HMI graphics/tearing refinement, and a plugin "locate"
> capability for the find-a-part flow. The find/locate behaviour is described
> in the ConOps and InvenTree sections but may still be evolving.
</content>
</invoke>
