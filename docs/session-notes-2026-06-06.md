# SmartReel — Session Notes 2026-06-06

Handoff for picking this up in a fresh session. Covers two big pieces shipped today:
the **production Core firmware** and **Stage 1 of the HMI dynamic-rack refactor**.

Related: `docs/smartreel-rs485-protocol.md` (wire protocol), plan file
`~/.claude/plans/warm-wibbling-parnas.md`, memory entries `core-fw-production`,
`hmi-slot-divider-model`.

---

## 1. Production Core firmware (`core-fw/`) — DONE, flashed, working on bench

`core-fw/` graduated from RS485 test rig to **production**. Drives real Core PCB hardware
via `src/reel_hw.{h,cpp}` + `src/config.h`, keeps the validated RS485 slave + arduino-pico
LittleFS OTA. Builds (11.4% flash) and flashed to the RP2040.

**Hardware model:** 4 PORTS (0–3), each with 1–4 chained reel MODULES. A module = 16
reel-slots + 16 dividers + 16 WS2812B LEDs + 32 input bits (interleaved `D0 S0 D1 S1 …`,
so `D_k = bit 2k`, `S_k = bit 2k+1`). Module count per port sensed from the ADC rail
voltage (2.2k pull-up ∥ 10k-per-module).

- **LEDs:** `Adafruit_NeoPXL8` (PIO+DMA, non-blocking — chosen so it can't corrupt an
  in-flight RS485 frame).
- **Inputs:** 74HC165 sampled ~100 Hz, per-32-bit-word debounce, edge → `INPUT_CHANGE`.
- **Sense:** ADS1115 round-robin ~10 Hz → module count + insert/remove events.
- **Debug LEDs:** GP2 = RS485 link health (solid <1 s, else 2 Hz blink), GP3 = activity.
- **Deferred:** `SET_ANIMATION` ACK-only; OTA stays simple (no A/B rollback).

**Wire format widened** (shared `rs485_proto.h`; HMI parser updated to match):
- `READ_INPUTS(port)` → `port, module_count, {u32 BE}×module_count`
- `INPUT_CHANGE` → `port, module_index, prev32(BE), new32(BE), ts_ms(BE)` (14 B)
- `GET_REEL_INFO` record → `port, present, sense_mv(2), module_count`
- `REEL_INSERTED` → `port, module_count, sense_mv`; `REEL_REMOVED` → `port, module_count`

**Bench TODO / watch:** verify module-count thresholds in `cfg::module_count_from_mv` at
the divider steps; confirm the 74HC165 module ordering assumption (module 0 = first 32 bits
clocked) — one-line flip in `read_chains_raw` if reversed.

---

## 2. HMI dynamic-rack refactor — STAGE 1 DONE (builds, NOT yet bench-verified)

Replaced the fixed 4×16 = 64-slot grid with a **dynamic rack** driven by live topology +
a **commissioned divider/slot layout**. HMI builds (flash 22.7%, RAM 43.7%). **Not flashed
yet** — needs a bench check.

### Model / commissioning (the agreed design)
- Build the system, configure the slot numbering + divider layout **once**, then it's frozen.
- **Pre-commission:** rack follows live hardware; physically pull a divider between two slots
  and they merge into one wider logical slot (the **lower** number wins, the upper number
  **disappears** as a gap, neighbours unchanged). Combining can straddle a module boundary
  within a port, never across ports.
- **Commission** (Configure → Divider Maintenance): snapshots live module counts + pulled
  dividers to `/sdcard/rack.json`.
- **Post-commission:** committed config is source of truth, numbering frozen; live deviations
  **alarm** (divider expected-present gone missing → `AnomalyKind::Divider`; module add/remove
  vs committed → Added/Removed warning).
- Numbering when not combined: compact, 1-based, port-ascending, skipping empty ports.

### Key files (Stage 1)
- `app/slot_map.{h,cpp}` — `SlotMap` base numbering + `LogicalRack` (merging), `DividerLayout`,
  bit-decode helpers, `validate_topology`.
- `app/hw_mirror.{h,cpp}` — live per-port module count + 32-bit words; `sync_from_core()`
  (direct blocking `get_reel_info` + `read_inputs`) at boot / topology change.
- `ui/app_state.{h,cpp}` — dynamic `rack[MAX_LOGICAL_SLOTS]` + `n_rack`; `Slot` carries
  width/port/module/mslot; `slot_at_physical`, `rebuild_rack`. **Dropped the random mock
  occupancy seed** — rack now reflects reality (empty until reels present / parts assigned).
- `main.cpp` — `on_rs485_event` re-pointed to the new 14-byte format (**fixes a regression
  where every INPUT_CHANGE was being silently dropped**). Boot: sync topology → `rebuild_rack`
  → `state_store::load` (now after topology is known).
- `app/leds.cpp` — logical slot → port/pixel span (wide slots light their whole span).
- `ui/widgets.cpp dot_grid_card` — dynamic per-port rows; combined slots render wider.
- `ui/screens/config_dividers.cpp` — real commission screen (status + Commission/Edit +
  logical-slot list).
- `storage/config_store.{h,cpp}` — `RackCfg` repurposed to `{committed, module_count[4],
  DividerLayout}`, persisted to `/sdcard/rack.json`.
- `storage/state_store.{h,cpp}` — rack contents persisted keyed by **logical number** (gaps
  allowed); occupancy is live/derived, not persisted.
- `ui/screen_manager` — added `mark_all_dirty()` to refresh screens on topology change.

### Bench verification checklist (do first next session)
1. Flash HMI (`./scripts/flash-hmi.sh`; needs manual boot-mode). Core already flashed.
2. Reel-presence switch → logical slot flips occupied/empty; no `INPUT_CHANGE payload` warning.
3. Divider Maintenance edit mode: pull a divider → two slots merge live (lower #, upper gone);
   Commission → persists.
4. Post-commission: pull a should-be-present divider → divider alarm; add/remove a module →
   warning.
5. Configure → Slots: live module counts + ranges.

---

## 3. Workflow status — what's REAL vs MOCK (important context)

Scan/place and pick flows EXIST and now run on real hardware (LEDs + reel switches via the
new event path), but three things are still mock/unwired across all of them:

- **Quantities are fake.** Placement uses `mock_place_reel` (random qty); picking marks a
  line done regardless of qty taken. No qty capture / no scale.
- **No InvenTree write-back.** `inv_api::assign_slot` / `pick_slot` / `clear_slot` are
  **implemented but never called**, and `app::Part` doesn't carry the resolved
  `stock_item_id`. Placements/picks persist locally (SD) only.
- **Pick jobs are mock-seeded** (`build_pick_jobs`), not fetched from InvenTree.

Specifically:
- **Load / scan-and-place:** QR scan + resolve is REAL (`inv_api::resolve_barcode`, SD
  `parts.json` fallback). "Place reel" lights empty slots; inserting a reel into a lit slot
  (real switch) or tapping the dot commits locally. Qty random; not synced.
- **Job picking:** Start lights target slots (pick-by-light); items complete by button or by
  physically pulling the reel from a lit TARGET slot (real). Locations resolved from local
  rack contents; jobs mock; qty unchecked; not synced; no auto-complete.
- **Single-part find-by-light:** Inventory (View) → "Find" lights the slot blue 3 s. Locate
  only (real LEDs), not a tracked pick.

---

## 4. Suggested next steps (pick up here)

1. **Bench-verify HMI Stage 1** (checklist above) before more building.
2. **Stage 2 — dynamic UI polish:** on-screen divider toggle (currently physical-pull only),
   wide-slot styling in view/pick/load, richer per-deviation alarm text, late-core-connect
   re-sync (today relies on boot sync + events).
3. **InvenTree wiring (the big functional gap):** carry `stock_item_id` + real `qty` from the
   resolve result into the slot; make place/pick call `assign_slot`/`pick_slot` and surface
   failures; fetch real pick jobs from the server; decide how qty is captured (from the stock
   item vs operator entry).

---

## Build / flash quick ref
- HMI: `cd esp32-hmi && ~/.local/bin/pio run` → `./scripts/flash-hmi.sh` (manual boot mode).
- Core: `cd core-fw && ~/.local/bin/pio run` → `./scripts/flash-core.sh` (UF2 to BOOTSEL).
- Ports: ttyACM0 = Pico (core), ttyACM1 = ESP32-S3 (HMI). Scripts locate boards by USB VID.
- Nothing committed yet this session (branch `hmi-inventree-client`).
