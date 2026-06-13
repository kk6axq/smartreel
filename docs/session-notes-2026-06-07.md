# SmartReel — Session Notes 2026-06-07

Handoff for a fresh session. Today was HMI/Core bring-up debugging on the bench:
fixed the PISO bit-order reversal, an HMI crash, the anomaly fault semantics, and the
load/self-test LED flows. Both boards flashed and tested on the bench.

Related: `docs/session-notes-2026-06-06.md`, memory entries `core-fw-production`,
`hmi-slot-divider-model`, `hmi-cpu-pinning`.

---

## Shipped today

### 1. PISO byte-order reversal (core-fw) — DONE, flashed
Dividers **and** reels read reversed within each group of 4. Root cause: each 8-channel
74HC165 (= 1 byte = 4 slots) clocks its four slots out in reversed order. Fixed in
`core-fw/src/reel_hw.cpp` `read_chains_raw()` via new `descramble_bit()`: per byte, reverse
the four (divider, slot) **pairs** while keeping each divider glued to its slot. (A literal
8-bit flip would swap D/S parity and decode reels as dividers — we reverse pairs, not bits.)
Byte/group order was already correct; only within-byte order is undone.

### 2. HMI crash on anomaly modal — DONE, flashed
`state-writer` task overflowed its 6 KB stack: `append_pending_anomalies()` had a
`char[16][240]` (~3.8 KB) on the stack and ran in the same iteration as a state write; the
combined FATFS depth tripped the stack canary (Guru Meditation, corrupted `0xa5a5a5a5`
backtrace). Fixed in `esp32-hmi/src/storage/state_store.cpp`: made that buffer `static`
(single-caller, single-task) and bumped the task stack 6 KB → 8 KB.

### 3. Inventory-aware removal fault — DONE
`esp32-hmi/src/main.cpp` `apply_slot_change()`:
- Insert without scan → non-latching "Added" warning.
- Remove an **inventoried** reel (`slot->part.valid`) → latching "Removed" fault (tamper).
- Remove an un-inventoried reel → undo the insert: reset slot, clear the "Added" warning,
  no fault.

### 4. Self Test screen — DONE
- **Buttons card**: live input watch. `main.cpp` routes raw module-word changes to
  `ui::screens::selftest_on_input()` while Self Test is current, and **skips the
  workflow/anomaly path** there (so testing buttons never raises/crashes the modal).
  Shows REEL/DIVIDER, port/module/slot, PISO reg+lane, bit, and decoded logical slot;
  mirrors each press to serial as `selftest: …`.
- **Single slot card**: slot number now editable (tap field → text-entry modal); Light/Off
  act on that slot. No more hardcoded slot 12.

### 5. Load placement LEDs + finish — DONE
`load.cpp` + `main.cpp`: tapping Place lights all open slots (`leds::light_target_slots()`);
placing a reel (physical insert **or** on-screen dot) registers the part, clears the LEDs,
and navigates Home. Cancel clears LEDs too.

### 6. LED lighting is single-pixel — DONE
`esp32-hmi/src/app/leds.cpp`: `light_slot()` and `light_where()` now light only the slot's
own physical position, not the full combined-run width (was lighting e.g. 5–16 for slot 5).

---

## NEXT SESSION — TODO

1. **Bigger fonts.** UI fonts are too small across screens. LVGL Montserrat sizes are set
   per-widget (`&lv_font_montserrat_12/14`) in `esp32-hmi/src/ui/` (theme.cpp, widgets.cpp,
   screens/*). Bump the base sizes; may need to enable larger Montserrat sizes in the LVGL
   config and audit fixed-height widgets/cards that could clip taller text.
2. **Rename "Configure" → "Settings".** The Configure screen/menu tile. Check
   `ui/screen_manager.cpp` (`title_for`, `Screen::Configure`), the home/menu tile label in
   `ui/screens/home.cpp` / `configure.cpp`, and any breadcrumbs. Consider whether to rename
   the `Screen::Configure` enum + `build_config*`/file names too, or just the display strings
   (display-string-only is the low-risk first pass).

## Watch / open questions
- Divider combining: live rack merges slots whose divider reads **absent**
  (`effective_topology` in `app_state.cpp`). On the bench with few physical dividers, slots
  combine into wide runs — expected, but confirm divider *presence polarity* is right if you
  expect them separate.
- Optional: on load placement, also report the registration to InvenTree (`net/inv_api.h`),
  and use a real qty instead of the mock `100 + rng()%900` in `mock_place_reel`.
