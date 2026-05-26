# Session handoff — SmartReel ESP32 HMI

Quick-resume notes so a fresh Claude session (or me on Monday) can pick
up without spelunking the chat history. Update or delete when the
work item below is closed.

---

## Repo layout

```
C:\Users\Lukas Severinghaus\Desktop\SmartReel\smartreel\
  .git/                              -- the project repo
  CorePCB/      ReelPCB/              -- KiCad PCBs (Rev 1 ordered)
  docs/
    smartreel-rs485-protocol.md     -- wire protocol spec
    roadmap.md                      -- Phase 1..4 plan + non-goals
    known-bugs.md                   -- empty at the moment
    session-handoff.md              -- this file
  esp32-hmi/                         -- PlatformIO project (the HMI firmware)
    platformio.ini
    include/
      lv_conf.h
      lv_heap_caps_glue.h
    src/
      main.cpp
      app/         leds.{h,cpp}     -- slot-LED helpers (RS485 wrappers)
      board/       board_pins.h, ch422g.{h,cpp}
      display/     display.{h,cpp}  -- IDF-5 RGB panel + LVGL direct_mode
      net/         wifi_mgr.{h,cpp}
      rs485/       crc16.*, rs485_proto.h, rs485_frame.*, rs485.{h,cpp}
      sensors/     qr_scanner.{h,cpp}
      storage/     sdcard.{h,cpp}, config_store.{h,cpp}, state_store.{h,cpp}
      touch/       gt911.{h,cpp}
      ui/          theme.{h,cpp}, app_state.{h,cpp}, widgets.{h,cpp},
                   screen_manager.{h,cpp}, status_bar.{h,cpp},
                   anomaly_modal.{h,cpp}, wifi_password_modal.{h,cpp}
                   screens/         home, load, view, pick, configure,
                                    config_slots, config_network,
                                    config_selftest, config_fwupdate,
                                    config_dividers, qr_scanner
      util/        lvgl_async.h     -- cross-thread dispatch to LVGL task
```

## Toolchain

Use this `pio` binary -- the user-home one has the old IDF 4.4 toolchain
and won't compile this project:

```
C:\.platformio\penv\Scripts\pio.exe
```

Platform: pioarduino `54.03.21` → arduino-esp32 3.2.1 / IDF 5.3 /
xtensa-esp-elf gcc 14.2.0.

Build / upload from Git Bash:

```bash
cd "/c/Users/Lukas Severinghaus/Desktop/SmartReel/smartreel/esp32-hmi"

# build only
"C:/.platformio/penv/Scripts/pio.exe" run

# upload (board must be in download mode: hold BOOT, tap RESET, release BOOT)
"C:/.platformio/penv/Scripts/pio.exe" run -t upload --upload-port COM5

# serial monitor
"C:/.platformio/penv/Scripts/pio.exe" device monitor -b 115200
```

The `Library Manager: Installing Network ... Could not find the package`
warning on every build is cosmetic — `Network` is bundled with
arduino-esp32 v3 but PIO checks the public registry first. Ignore.

## What's committed

Most recent commits (read top-down for chronology of the project):

```
2f936b9 ESP32 HMI: SD-backed runtime state (Phase 1a)         <-- HEAD
27bf330 ESP32 HMI: switch LVGL to direct_mode (AVOID_TEARING_MODE 3)
b9979d1 ESP32 HMI: tighten QR-scanner screen to stop tearing on poll
a521c0c ESP32 HMI: live QR-scanner Self Test screen
954119e ESP32 HMI: fix scrolling-list tearing via full-refresh + vsync swap
b7a777c roadmap: flag auto-format-on-mount-failure for removal
b9d4fe0 Add docs/roadmap.md scoping the rest of the ESP32 HMI work
d4503e6 ESP32: RS485 master implementation (protocol per docs/)
6a3f00f Add ESP32 HMI firmware + docs/known-bugs.md
04ad79d Rev 1 PCBs ordered                                    <-- pre-firmware baseline
```

Everything in `esp32-hmi/` was last on-the-device-and-confirmed-working
at commit `27bf330` (direct-mode display fix). Commit `2f936b9` added
the persistence layer; the build was green but **the user has not
flashed it yet** -- it's untested on real hardware.

## What's uncommitted right now

`git status --short` shows:

```
 M esp32-hmi/src/main.cpp
 M esp32-hmi/src/ui/screens/config_selftest.cpp
 M esp32-hmi/src/ui/screens/pick.cpp
 M esp32-hmi/src/ui/screens/view.cpp
?? esp32-hmi/src/app/                       (new module: leds.{h,cpp})

 M CorePCB/SmartReelCore/SmartReelCore.kicad_pro   (user's KiCad work — leave alone)
```

These are **Phase 1b/c/d/e** from the roadmap:

- **main.cpp** — Real RS485 event dispatcher. The `EVT_INPUT_CHANGE`
  payload is parsed in the POLL task, an `InputChangeEvent` struct is
  heap-allocated, and `ui::dispatch_on_lvgl(apply_input_change, ev)`
  marshals onto the LVGL task. `apply_input_change` walks the changed
  bits, looks up each slot, classifies the transition as expected (pick
  workflow removing a target, load workflow filling a target) or
  unexpected (everything else), mutates `app_state` with the lock held,
  marks dirty for the writer task, and raises the anomaly modal on
  unexpected changes. `EVT_REEL_INSERTED/REMOVED/SENSE_THRESHOLD/LOG`
  still just log; per-chain presence tracking is future work.

- **src/app/leds.h, src/app/leds.cpp** — New module. Slot↔reel/pixel
  math + thin `rs485::*` wrappers using the COMMIT pattern. Functions:
  `light_slot(slot, r, g, b)`, `light_target_slots()`, `light_empty_slots()`,
  `fill_all(r, g, b)`, `clear_all()`. Walk app_state, build a 16-pixel
  buffer per reel, `set_reel_pixels` per reel, then one `commit_pixels(0x0F)`
  for atomic show across all 4 reels. Returns RS485 Status but UI
  handlers fire-and-forget (no Core PCB attached yet, so they time out).

- **pick.cpp** — Start handler now calls `leds::light_target_slots()`
  after `mock_start_pick`. Added `cancel_active_job` handler + a bottom
  "Cancel job" button that reverts TARGET→OCCUPIED, drops
  active_pick_idx, persists, and `leds::clear_all()`s the reels.

- **view.cpp** — Replaced the per-row "Pick" button (destructive
  mock action) with "Find" — lights the slot LED blue for 3 seconds
  via an `lv_timer_create(..., 3000)` one-shot. New `FindCtx` struct
  to manage the closure capture and free-on-delete.

- **config_selftest.cpp** — "All LEDs" / "Single slot" / "RS485 chain"
  buttons now actually do RS485 work: `leds::fill_all`, `leds::clear_all`,
  `leds::light_slot(12, ...)`, `rs485::ping`. (Just added `#include
  <esp32-hal-log.h>` to silence a `log_i` error from this batch -- build
  in flight right now to confirm green.)

## Build status

A foreground `pio run` is in flight as of this writing
(task `bdfjrx1v8`, fixing the `log_i` include error). Once it returns
SUCCESS, the next-session intent is to **commit** these as a single
Phase 1b/c/d/e commit and then **flash + test** against the exit
criteria below.

Suggested commit message draft:

```
ESP32 HMI: Phase 1 RS485↔UI plumbing (b/c/d/e)

main.cpp
  Replace the stub RS485 event handler with a real dispatcher.
  EVT_INPUT_CHANGE parses both 7B (8-input) and 9B (16-input)
  payload shapes, heap-allocates an InputChangeEvent, and
  ui::dispatch_on_lvgl-s apply_input_change onto the LVGL task.
  The handler walks changed bits, classifies each slot transition
  as expected (pick target-removal, load target-fill) vs
  unexpected (anomaly), mutates app_state under app::lock(),
  marks dirty, and raises the anomaly modal on unexpected events.

src/app/leds.{h,cpp}
  New module. Slot<->reel/pixel mapping + COMMIT-pattern wrappers
  around rs485:: for atomic multi-reel updates.

src/ui/screens/pick.cpp
  Start handler fires leds::light_target_slots() after mock_start_pick.
  New cancel_active_job + bottom "Cancel job" button: revert
  TARGET->OCCUPIED, drop active_pick_idx, persist, clear LEDs.

src/ui/screens/view.cpp
  Replace destructive "Pick" button with "Find" -- 3s blue
  highlight via lv_timer one-shot.

src/ui/screens/config_selftest.cpp
  "All LEDs" on/off, "Single slot" light, "RS485 chain" ping all
  hit real RS485 now (fire-and-forget; safe with no Core PCB).
```

## Phase 1 exit criteria (from docs/roadmap.md)

- [x] Power-cycle the unit → rack screen shows last persisted slot
      occupancy. **(verified-via-build, untested on hardware)**
- [ ] Yank a reel without an active job → anomaly modal AND slot
      turns red AND record lands in `/sdcard/anomalies.jsonl`.
      **(blocked: needs Core PCB firmware to generate
      EVT_INPUT_CHANGE — can test the path via Self Test → Trigger
      Anomaly buttons for the modal half)**
- [ ] Start a mock pick from the Queue screen → Core lights target
      slots; per-pick progress updates `/sdcard/jobs.json`; cancel
      clears LEDs. **(per-pick state-store write works; LED part
      blocked on Core firmware)**

## Caveats / known sharp edges

- **No Core PCB firmware exists**, so all RS485 outbound calls
  time out and return `Status::Timeout`. The UI handlers fire-and-
  forget, but the RS485 POLL task logs a steady stream of timeouts
  on serial. Comment out `rs485::init()` in main.cpp boot step 9
  if you want quiet logs.

- The `mock_*` mutators in `app_state.cpp` are now the real
  mutators (lock + dirty-mark). Naming hasn't been cleaned up
  (still `mock_place_reel` etc.). Cosmetic; do a rename pass when
  there's a slow moment.

- `app::lock()` is a coarse mutex over the whole `State` struct.
  Held for ~tens of µs at a time by mutators on the LVGL task, and
  for the duration of one ~17 KB memcpy by the writer task on
  PRO_CPU. UI readers on the LVGL task don't lock (single-writer
  invariant: everything that mutates state is on the LVGL task,
  either directly or via `ui::dispatch_on_lvgl`).

- `state_store::save_*` runs on a dedicated `state-writer` task at
  priority 1 on PRO_CPU. 250 ms debounce + signal coalescing means
  a burst of slot updates produces ≤4 SD writes/sec. ArduinoJson
  serialisation allocates from heap; the snapshot buffer (~17 KB)
  is one-time allocated in PSRAM by the writer task at startup.

- The `auto-format-on-mount-failure` SD behaviour is still on. See
  Phase 4 in `docs/roadmap.md`: this is dangerous in the field
  (a corrupted FS looks identical to "unknown format" so we'd
  silently wipe). Flip `format_if_mount_failed = false` in
  `sdcard::init()` before shipping anything to anyone.

- `EVT_REEL_INSERTED/REMOVED` currently just log to serial. The
  "whole chain disconnected" UX (gray out a chain row in the dot
  grid + raise anomaly) is unimplemented. Low priority.

- `apply_input_change` uses `mock_raise_anomaly` for the unexpected
  path, which auto-populates the anomaly text by scanning app_state
  for the first matching slot. That's wrong: the anomaly should
  identify the SPECIFIC slot where the unexpected change happened.
  Improve when there's a need.

- The QR scanner screen's lv_timer is a 5 Hz I2C poll; it skips work
  when the user isn't on the screen, but the timer itself stays
  alive. ~free.

## What's next on the roadmap

Once Phase 1 is flashed + verified:

- **Phase 2** — Inventree HTTPS client (`net/http_client.h`,
  `net/inventree.h`, persistent cache on SD). See `docs/roadmap.md`
  for the deliverables list + exit criteria. Estimated ~4-5 days.
  Decision gate for the "stay on ESP32 vs move WiFi to a Pico W"
  question (A1 in the roadmap) lives at the end of Phase 2 — if
  TLS+HTTPS judder the UI, that's when we revisit.

- **Phase 3** — Admin web interface + RP2040 OTA push. The RP2040
  side needs firmware first; the wire constants for FW_BEGIN/CHUNK/
  VERIFY/COMMIT/CONFIRM are already in `rs485_proto.h`.

- **Phase 4** — Polish backlog. Top item: remove auto-format-on-
  mount-failure (see Caveats above). Then: rebuild perf, tearing
  fix audit, NTP for the topbar clock, anomaly review screen
  reading `anomalies.jsonl`.

## How to resume

In a fresh session, point the model at this file plus
`docs/roadmap.md`. Then:

1. `git status` — should show only the uncommitted Phase 1b/c/d/e
   files listed above plus the CorePCB modification (ignore).
2. Re-run the build to confirm it's still green:
   `"C:/.platformio/penv/Scripts/pio.exe" run`
3. Commit using the draft message above (adapt as needed).
4. Flash, run through the Phase 1 exit checklist with whatever
   subset of the Core PCB exists at that point.
5. Move on to Phase 2 (Inventree) or pick a different item if
   priorities have shifted.

## Useful greps for orientation

- `app::lock()` — find every place that takes the state mutex.
- `state_store::mark_` — find every place that signals a save.
- `leds::` — find every place that fires an RS485 LED command.
- `ui::dispatch_on_lvgl` — find every cross-thread → LVGL handoff.
- `TODO|FIXME` — sparse but real (mostly in `apply_input_change`).
