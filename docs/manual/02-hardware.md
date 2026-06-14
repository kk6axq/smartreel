# SmartReel — Hardware Structure

This describes the physical/electrical structure of a SmartReel rack. Details
come from the firmware configuration, board READMEs, and the KiCad project file
names; where a value couldn't be confirmed from text sources it is marked TODO.

## 1. Overview

A rack is three kinds of board on one cable run:

```
  +-----------------------+        +------------------+        +-----------------------------+
  |  HMI                  |        |  Core PCB        |        |  Reel modules (per port)    |
  |  ESP32-S3 +           | RS485  |  RP2040/RP2350   | ports  |  WS2812B LEDs               |
  |  4.3" RGB touchscreen |<------>|  bus slave       |<======>|  74HC165 input registers    |
  |  + SD card            |        |  bus master-side |  0..3  |  slot + divider buttons     |
  |  (RS485 master)       |        |  to modules      |        |  sense/ID resistor network  |
  +-----------------------+        +------------------+        +-----------------------------+
```

- The **HMI** is the RS485 bus master and the user interface.
- The **Core PCB** is the RS485 slave; it drives up to 4 ports of reel modules.
- **Reel modules** are pluggable and chain off each port (up to 4 deep).

Capacity ceiling: **4 ports × 4 modules/port × 16 slots/module = 256 slots**.

## 2. HMI — ESP32-S3 touchscreen

Board: **Waveshare ESP32-S3-Touch-LCD-4.3B**.

| Item | Detail |
|---|---|
| MCU | ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM), 240 MHz |
| Display | 4.3", 800×480, 16-bit RGB565 parallel (DPI), ST7262-class timing |
| Pixel clock | 16 MHz (12 MHz causes a stuck-white panel on this board — do not lower) |
| Touch | GT911 capacitive, I²C (addr 0x5D/0x14), INT on GPIO4 |
| I/O expander | CH422G on I²C (addr 0x24) — drives LCD reset, backlight, touch reset, USB select |
| Storage | microSD via SDMMC, **1-bit** mode (CMD GPIO11, CLK GPIO12, D0 GPIO13) |
| RS485 | UART, **auto-direction** transceiver — TX GPIO44, RX GPIO43, no DE pin |
| I²C bus | SDA GPIO8, SCL GPIO9, 400 kHz (shared by GT911, CH422G, optional QR scanner) |
| QR scanner | optional I²C device (addr 0x0C); if absent the HMI falls back to manual entry |

RGB LCD pin map (from `esp32-hmi/src/board/board_pins.h`):

```
HSYNC GPIO46   VSYNC GPIO3   DE GPIO5   PCLK GPIO7
R[0..4] GPIO 1,  2, 42, 41, 40
G[0..5] GPIO 39, 0, 45, 48, 47, 21
B[0..4] GPIO 14, 38, 18, 17, 10
```

The display uses **two framebuffers in PSRAM** (~768 KB each, ~1.5 MB total)
for tearing-free refresh; see [Software Architecture](03-software-architecture.md#display).

> **RS485 direction asymmetry.** The HMI side uses a hardware *auto-direction*
> transceiver (no driver-enable GPIO), and the firmware caps the link at
> **115200 baud** because the auto-direction RC switch cannot turn the line
> around fast enough for higher rates. The Core side (below) uses a discrete DE
> pin. Both ends run 115200.

## 3. Core PCB

KiCad project: `pcbs/CorePCB/SmartReelCore/`.

| Function | Part / detail |
|---|---|
| MCU | Raspberry Pi Pico class — **RP2040 or RP2350** |
| RS485 transceiver | SP3485 (half-duplex) with a discrete DE/RE# line |
| Level shifting | SN74AHCT244 / 74LVC244 (3.3 V ↔ 5 V) for the module signals |
| Module rail | 12 V → 5 V buck converter feeding the reel modules |
| Sense ADC | ADS1115 (16-bit I²C, 4 channels — one per port) for module detection |

Core pin assignments (from `core-fw/src/config.h`):

```
RS485:   TX GP16 -> SP3485 DI     RX GP17 <- SP3485 RO     DE GP18 -> SP3485 DE+RE#
RS485 baud: 115200
LED data (NeoPXL8): GP6, GP7, GP8, GP9  (ports 0..3)
Shift registers (74HC165), shared:  clock GP10,  latch/PL# GP11 (active LOW)
Shift-register data in, per port:   GP12, GP13, GP14, GP15
Sense ADC: ADS1115 on I²C0 (SDA GP0/SCL GP1 area — TODO confirm exact I²C pins), addr 0x48
Debug LED: GP2 (RS485 link health)
```

The Core is RS485 slave address **0x01**. It declares the link "up" only while a
valid master frame has arrived within the last 1000 ms.

## 4. Reel modules

KiCad project: `pcbs/ReelPCB/` (with a `Switch_Unit.kicad_sch` sub-circuit and
per-module label PDFs under `labels/`).

Each module provides **16 reel slots** and carries:

- **16 WS2812B addressable RGB LEDs** — one per slot, daisy-chained. Driven from
  the Core via NeoPXL8 (PIO + DMA, 8 parallel data lines, 800 kHz, GRB order).
- **Inputs via 74HC165 PISO shift registers** — **32 input bits per module**:
  - 16 **slot-present** bits (a button pressed while a reel sits in the slot), and
  - 16 **divider** bits (a button pressed while the divider to the *right* of a
    slot is installed).
  - Logical bit order per module is `D0 S0 D1 S1 … D15 S15`. (The raw hardware
    clocks slots out reversed within each shift-register byte; the Core firmware
    descrambles them, so the rest of the system sees the clean order.)
- **Sense / ID resistor network** — a per-module pull-down that the Core reads on
  that port's ADS1115 channel to count how many modules are chained.

### Dividers and slot width

Dividers between slots are removable. Pulling the divider between two slots
merges them into one wider logical slot (for a larger reel). Because each
divider has a sense bit, the rack automatically knows the width of every slot.
When several physical slots form one composite slot, all of their LEDs follow
the same colour/state together.

### Module-count detection

Each inserted module adds a 10 kΩ pull-down in parallel against a 2.2 kΩ
pull-up to 3.3 V, so the rail voltage on a port quantises by module count. The
Core firmware maps the measured millivolts to a module count (0–4):

| Modules | Approx. sense (mV) |
|---|---|
| 0 (open) | 3300 |
| 1 | 1458 |
| 2 | 989 |
| 3 | 732 |
| 4 | 579 |

A change must hold for 2 consecutive samples before the Core emits a
`REEL_INSERTED`/`REEL_REMOVED` event.

## 5. Ports and chaining

- The Core has **4 ports** (0–3). In the protocol, `reel_id` **is the port
  number**.
- Each port carries one LED data line, one shift-register chain (shared clock +
  latch, per-port data line), and one sense channel.
- Up to **4 modules chain per port**. Within a port, module `m` owns LED pixels
  `[m*16, m*16+16)` and input bits `[m*32, m*32+32)`.
- The HMI labels ports as "chains" 1–4 in the UI.

## 6. Power

- **12 V** input is the main rail. The Core PCB bucks it to **5 V** for the reel
  module rail (LEDs + logic). Reel modules use 5 V and 3.3 V (3.3 V generated on
  the Core / modules — TODO confirm exact per-module regulation).
- **HMI power**: the Waveshare board is typically powered over USB-C or a 5 V
  input. The exact rack-integration power source is TODO (not documented in the
  source tree).

## 7. RS485 wiring

A single RS485 pair runs between the HMI (master) and the Core (slave). The Core
fans the reel modules out over its ports; the module signals (LED data,
shift-register clock/latch/data, sense) are *not* RS485 — they are local
parallel/serial lines level-shifted on the Core PCB. Only the HMI↔Core link is
RS485.

- Master address `0xFF`, Core address `0x01`, broadcast `0x00`.
- 115200 baud, half-duplex. The HMI polls the Core every ~20 ms so button
  events reach the UI within ~50 ms.

See the [API Reference](04-api-reference.md#part-b-rs485-wire-protocol) for the
full frame format and message catalog.

## 8. Supporting artifacts in the repo

- `labels/` — printable slot-label PDFs (one per module, slots numbered).
- `scripts/` — flashing, SD deployment, and mock-QR generation helpers.
- `debug-fw/inject.py` — bench event injector for the RP2040 core test rig.
- `debug-fw/sd-assets/` — mock QR codes for bench-testing the scan flow.
- `pcbs/CorePCB/`, `pcbs/ReelPCB/` — the KiCad designs (schematics, PCB, production
  outputs under each project's `production/`).

## 9. Known hardware TODOs

- Exact ADS1115 I²C pin assignment on the Core (config comment vs. board).
- Per-module 3.3 V regulation source.
- HMI power input in the final rack integration.
- Real-hardware validation of the module-count sense thresholds.
</content>
