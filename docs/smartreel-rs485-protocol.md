# SmartReel RS485 Protocol & Firmware Update Design
SmartReel — Project Context
A modular system of "smart reels" managed from a touchscreen HMI. Each reel module has addressable LEDs and a row of inputs; the HMI controls the lighting and reacts to input events.
Hardware architecture
Three boards, one cable run:

HMI — ESP32 with touchscreen + SD card. Acts as RS485 bus master. User interface, configuration storage, firmware images for the core, any network connectivity.
Core PCB — RP2040 or RP2350 ("Pico"). Talks to up to 4+ reel modules. Hosts the RS485 transceiver (SP3485), 3.3V↔5V level shifters (74AHCT244 / 74LVC244), and a 12V→5V buck for the reel rail.
Reel modules — pluggable. Each carries WS2812B LEDs, a 74HC165 PISO shift register for inputs, and a sense resistor/ID network read by the core's ADC.

ESP32 HMI ↔ RP2040/RP2350 core, RS485 link, four reel modules with WS2812B LEDs and PISO shift registers.

## Frame format

Length-prefixed frame with CRC-16. Avoids the timing dependencies of Modbus RTU and is easier to debug:

```
+------+------+------+------+------+------+------+--------------+------+------+
| 0xAA | 0x55 | LEN_H| LEN_L| ADDR | SEQ  | TYPE | PAYLOAD ...  | CRC_H| CRC_L|
+------+------+------+------+------+------+------+--------------+------+------+
```

- **0xAA 0x55** — sync bytes; receiver hunts for these to resync after a glitch.
- **LEN** — number of bytes from ADDR through end of PAYLOAD (excludes sync and CRC). 16-bit lets you do larger firmware chunks comfortably.
- **ADDR** — slave address (0x01 for the core; reserve 0x00 broadcast, 0xFF for the ESP32-as-master). Useful if you ever bus more than one core, or for a "scan for devices" probe.
- **SEQ** — sequence number; a response echoes the request's SEQ so the ESP32 can match them and detect retransmits.
- **TYPE** — high nibble = category (0=system, 1=reel I/O, 2=LED, 3=config, 8=async event, F=firmware update); low nibble + payload define the specific command. Lets you parse/dispatch by category cheaply.
- **CRC-16/CCITT** over ADDR..end-of-PAYLOAD. CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) is fine and trivially available on both sides.

Replies use the same TYPE byte with bit 7 set (so 0x10 request → 0x90 response), with bit 6 indicating error (so 0xD0 = error response). That makes the wire trace readable without a decoder table.

## Application message catalog

### System (TYPE 0x0_)

| Code | Name | Dir | Payload |
|------|------|-----|---------|
| 0x00 | PING | E→P | none → echo |
| 0x01 | GET_VERSION | E→P | none → fw_major, fw_minor, fw_patch, build_id (4B), hw_rev |
| 0x02 | GET_STATUS | E→P | none → uptime_s (4B), reels_present (bitmap), error_flags (2B) |
| 0x03 | RESET | E→P | reason byte (0=normal, 1=enter_update_mode) |
| 0x04 | POLL | E→P | none → "no events" or up to N queued events |

### Reel I/O (TYPE 0x1_)

| Code | Name | Dir | Payload |
|------|------|-----|---------|
| 0x10 | GET_REEL_INFO | E→P | reel_id (or 0xFF for all) → presence, sense_mv (2B per reel), eeprom/ID bytes |
| 0x11 | READ_INPUTS | E→P | reel_id → PISO bytes (whatever your shift register reads) |
| 0x12 | SET_POLL_RATE | E→P | reel_id, rate_hz (2B) — how often the Pico samples PISO |

### LED control (TYPE 0x2_)

| Code | Name | Dir | Payload |
|------|------|-----|---------|
| 0x20 | SET_REEL_PIXELS | E→P | reel_id, start_idx, count, R,G,B,R,G,B,... |
| 0x21 | FILL_REEL | E→P | reel_id, R, G, B |
| 0x22 | SET_BRIGHTNESS | E→P | reel_id (or 0xFF), brightness (0–255) |
| 0x23 | SET_ANIMATION | E→P | reel_id, anim_id, params... (chase, pulse, etc — runs locally on Pico) |
| 0x24 | COMMIT | E→P | reel_bitmap — atomic show of staged pixel data across multiple reels |

The COMMIT pattern is worth doing: SET_REEL_PIXELS writes to a back-buffer, COMMIT flips and calls `show()` across all selected reels in the same frame. Otherwise your reels visibly tear when updating more than one at a time over a slowish bus.

### Async events (TYPE 0x8_, Pico → ESP32, no response expected)

| Code | Name | Payload |
|------|------|---------|
| 0x80 | INPUT_CHANGE | reel_id, prev_state, new_state, timestamp_ms (4B) |
| 0x81 | REEL_INSERTED | reel_id, sense_mv (2B), ID bytes |
| 0x82 | REEL_REMOVED | reel_id |
| 0x83 | SENSE_THRESHOLD | reel_id, sense_mv, threshold_id |
| 0x8F | LOG | level, ASCII message |

Events are fire-and-forget — but the ESP32 should NAK with an "unknown SEQ" if it gets a spurious one, so the Pico knows the link is alive. Pico keeps a small ring buffer of unACK'd events and resends after a timeout; mark them with a sequence number the ESP32 echoes back in an ACK frame.

This is important: **on a half-duplex RS485 link, the Pico cannot transmit whenever it wants**. The ESP32 is bus master. So async events have to be either (a) returned in the response slot of the next poll, or (b) signaled via a dedicated "any events pending?" poll the ESP32 issues regularly. The simplest pattern: the ESP32 issues `POLL` (0x04) every ~20ms, and the Pico's response is either "no events" or up to N queued events. Higher-priority button events should keep latency under ~50ms; this poll rate gets you there.

## Firmware update

Run a custom bootloader on the Pico and lay out flash as A/B slots so you always have a known-good fallback.

### Flash layout (RP2040, 2 MB)

```
0x10000000  Bootloader            (32 KB)
0x10008000  Metadata block        (4 KB, two copies for wear)
0x1000A000  App Slot A            (~1000 KB)
0x10104000  App Slot B            (~1000 KB)
0x101FE000  Reserved / config     (8 KB)
```

On RP2350 (4 MB) you have headroom to make each slot 1.5 MB and add a scratch staging area if you want, but the A/B pattern is the same.

### Metadata block

Contains: active slot, pending slot, image size, SHA-256, version, boot count, "needs confirm" flag. Write two copies and pick the one with the higher monotonic counter — cheap protection against power-loss during update.

### Bootloader logic

On reset, read metadata, pick the slot marked active, verify its hash if `needs_confirm == true`, jump to it. If the previously-booted slot failed to clear the `needs_confirm` flag within N boots, revert to the other slot. This is the rollback guarantee.

### Update flow over RS485

| Step | Message | Notes |
|------|---------|-------|
| 1 | `FW_BEGIN` | ESP32 sends total_size (4B), SHA-256 (32B), version (4B). Pico picks inactive slot, erases it (this takes a few seconds — respond with `BUSY` until done, then `READY`). |
| 2 | `FW_CHUNK` × N | offset (4B) + data (up to ~512B per frame). Pico writes to flash, ACKs with the offset it last wrote. ESP32 retries any unACK'd chunk. |
| 3 | `FW_VERIFY` | Pico re-reads the slot, hashes it, compares to the announced hash. Returns OK or hash-mismatch. |
| 4 | `FW_COMMIT` | Pico writes metadata: pending=new_slot, needs_confirm=true. ACKs, then issues itself a reset. |
| 5 | Bootloader runs | Sees pending slot, boots it, sets boot_count++. |
| 6 | New app boots | Sends `FW_BOOTED` event to ESP32. ESP32 sends `FW_CONFIRM`, which clears `needs_confirm`. Without this confirm within a deadline, next reset reverts to the previous slot. |

The confirm-after-boot step is what saves you when the new firmware boots but is broken in a way that crashes before it can talk to the HMI — you ship a unit, update over the wire, the new firmware has a bug that bricks RS485, and on the next power cycle the bootloader rolls back automatically.

### Practical numbers

- At 115200 baud, raw throughput is ~11 KB/s; a 500 KB image takes ~50 s plus erase/verify overhead. Bump to 921600 baud (SP3485 handles it fine over short cable runs) and you're at ~6 s.
- Chunk size: 256 B is the RP2040 flash page size. Send one or two pages per RS485 frame so the Pico can write a whole page without buffering. With 512 B chunks + framing overhead, you get ~80% bus efficiency.
- During update, suspend reel polling and LED animations — the Pico shouldn't be doing PIO work while it's erasing flash anyway (XIP is disabled, interrupts off).

### Entering update mode

Two options:

1. The running app handles `FW_BEGIN` itself (writing to the *other* slot while it runs from its own), which is the smoothest UX.
2. The ESP32 sends `RESET` with reason=enter_update_mode, the bootloader sees a flag in metadata, and runs a minimal RS485 update handler instead of jumping to the app.

The first is preferable because the user gets reel functionality up until the moment of commit; the bootloader fallback is your insurance for when the app is too broken to even handle FW messages.

One last thing — keep the bootloader *small* and *boring*. No reel logic, no LEDs, no animations. Just RS485 receive, flash write, metadata, jump. That bootloader is the thing you can never field-update, so it has to be obviously correct.
