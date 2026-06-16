# Review Feedback — Proposed Solutions

Each heading is a line item from `review-feedback.md`. The proposed solution below
it references the actual code paths so it can become a work ticket. Numbering matches
the original feedback lines.

Cross-cutting note: most "confirmation" items below want the same reusable building
block — a lightweight **confirm/toast modal** on `lv_layer_top()`, modeled on
`anomaly_modal.cpp` (backdrop + box + header + body + 1–2 action buttons). Building
that once (call it `confirm_modal` / `toast`) unblocks items 3, 4, 7, 8, 16. It's
listed as its own pre-req in item 4.

---

## 1. View and Rack are not intuitively distinct — make Rack a sub-page of View

Today `Screen::View` (list of occupied slots, `screens/view.cpp`) and
`Screen::RackGrid` (read-only dot-grid overview) are both top-level tiles launched
from Home (`screen_manager.cpp`). They overlap conceptually.

**Proposed solution:**
- Remove the RackGrid tile from `build_home()`.
- Add a "Rack overview" button in the View screen header that calls
  `navigate(Screen::RackGrid)`. Since `navigate()` pushes a history stack, the back
  button already returns RackGrid → View correctly.
- Optionally rename the Home tile "View" → "Inventory" or "Reels" so the top-level
  label describes contents, and let the grid live one level down as the spatial map.
- No enum changes needed; this is pure navigation wiring in `home.cpp` + `view.cpp`.

## 2. On illegal removal, light the slot — flash the LED red

Illegal removal is already detected in `apply_slot_change()` (`main.cpp`), which sets
`slot->state = WARN` and `AnomalyKind::Removed`. LEDs are not currently flashed.

**Proposed solution:**
- On raising the `Removed` anomaly, drive the offending slot's LED with
  `leds::light_slot(slot_num, 0xEF, 0x44, 0x44)` (red).
- Add a flashing behavior: a periodic LVGL/timer tick (or the existing UI poll) that
  toggles the slot between red and off at ~1–2 Hz while the anomaly is unresolved.
  A small `g_flash_slot` + phase variable, cleared in `mock_resolve_anomaly()`, keeps
  it simple. Reuse the locate-overlay pattern as a precedent for periodic LED writes.
- Clear the LED back to its committed state when the anomaly is resolved/dismissed.

## 3. Illegal inventory unload on "resolve" needs an explicit confirmation

> **CONFIRMED BUG — must fix.** Resolving a replaced reel must **never** drop the part
> from inventory. This is a correctness defect, not just a missing confirmation dialog.

Reported: a reel was illegally moved + replaced, user hit "Mark resolved," and the
part got unloaded from inventory. `anomaly_modal_resolve()` should only *acknowledge*
the anomaly (`mock_resolve_anomaly()` clears `anomaly.kind`); the inventory clear is
a separate `queue_clear()`. The two are being conflated in the removed-flow, so
acknowledging an anomaly is silently destroying stock.

**Proposed solution:**
- **Primary fix:** in the removed-reel flow (`apply_slot_change()` / `anomaly_modal.cpp`),
  ensure resolving/acknowledging the anomaly does **not** call `queue_clear()`. The
  default, non-destructive path must keep the part loaded.
- Separate the two intents in the anomaly modal for `Removed`: offer
  **"Reel replaced — keep loaded"** (acknowledge only, no `queue_clear`) vs
  **"Unload from inventory"**.
- Gate the unload path behind a second confirm step ("Are you sure you want to unload
  this part from inventory? This removes it from stock.") using the new confirm modal.
- Audit `apply_slot_change()` / `anomaly_modal.cpp` so that resolving a *replaced* reel
  never auto-fires `queue_clear()`. Default action should be the non-destructive one.

## 4. Confirm a successful load — flash the slot LED green for ~2s

Today a successful load just advances to the placed view and returns Home
(`on_dot_pick()` → `app::mock_place_reel()`); there's no explicit success cue.

**Proposed solution (also builds the shared confirm/toast modal):**
- After `mock_place_reel(slot_num)` succeeds, drive
  `leds::light_slot(slot_num, 0x16, 0xA3, 0x4A)` (theme green) for 2s via a one-shot
  timer (same one-shot pattern used by View's `on_find()`), then restore committed color.
- Show a brief success toast ("Loaded to slot #N") via the new toast modal, auto-
  dismissing after ~2s, before returning Home.

## 5. Add beeper hardware (user-supplied) — wire confirm/error tones

Hardware is the user's task. We prepare the software seam.

**Proposed solution:**
- Add a small `app/beeper` module with a tone API (`beeper::ok()`, `beeper::error()`,
  `beeper::warn()`) that no-ops until the GPIO/PWM pin is known.
- Define semantic hook points now: success load (item 4), pick complete (items 7, 16),
  scan error (item 20), illegal removal (item 2), refresh complete (item 8). Have those
  sites call the beeper API so wiring tones later is a one-file change.
- Respect CPU pinning: any tone task pins to `APP_CPU` (never `PRO_CPU`, per project rule).

## 6. Detect when a part is moved out of a slot in InvenTree (to staging) within 10s

Server-side moves are caught by `apply_rack_result()` (`inv_sync.cpp`), but the rack
reconcile only runs every **60s** (`RACK_IVL_MS`) and only when the op queue is empty.
That misses the 10s requirement.

**Proposed solution (approach confirmed):**
- Add a lightweight occupancy-hash poll: a new `GET /rack?since=<rev>` / occupancy-hash
  endpoint in the plugin that the HMI polls at ~5s. Only run the full `apply_rack_result`
  reconcile when the hash changes. This bounds server load and meets the 10s SLA.
  (Considered and rejected: simply shortening the full reconcile to 5–10s — too much
  load for every rack on every poll.)
- Keep the "only reconcile when queue empty" guard so local mutations still settle first,
  but allow the *detection* poll to run regardless and just defer the apply if ops pend.
- On detecting the move, light the slot amber (already used at `inv_sync.cpp` for moved
  reels) and raise the appropriate reconcile prompt.

## 7. Confirm completion of a manual pick

Manual pick-out (`view.cpp` `on_pick()` → physical removal → `finish_pick_out()`) ends
silently aside from the LED darkening.

**Proposed solution:**
- In `finish_pick_out(slot_num)`, fire a success toast ("Picked from slot #N") via the
  confirm/toast modal and a beeper `ok()` tone (item 5).
- Same cue for job-driven picks in `pick.cpp` `on_picked()`.

## 8. Refresh button on pick screen may not work — confirm on refresh

`on_jobs_refresh()` calls `inv_sync::request_jobs_refresh()`, which is throttled (10s)
and gives no user feedback, so it can look dead.

**Proposed solution:**
- Give the refresh button immediate visual feedback: disable + spinner/"Refreshing…"
  state on tap, restored when the jobs list actually updates (or after timeout).
- Pass `force=true` from an explicit user tap so it bypasses the 10s throttle.
- Show a toast on completion ("Up to date" / "N jobs"). Verify `request_jobs_refresh`
  is actually wired to re-render the list on result, not just enqueue.

## 9. (blank line — no item)

## 10. Build-order plugin screen should handle multiple SmartReels and stock-item selection

Currently `renderBuildPanel()` (`static/panel.js`) creates one job per build targeting
**one** rack chosen from a dropdown, with one job item per BOM line (no stock-item
granularity). The HMI lights all slots holding that part.

**Proposed solution:**
- **Stock-item selection:** when a BOM part has multiple in-stock items/reels, expand
  the panel row to let the user pick which stock item(s) to pull (checkbox list per
  part). Plumb the chosen `stock_id`(s) into the job item in `create_job_from_build`
  (`services.py`) instead of resolving "any slot with this part" at pick time.
- **Multi-rack dispatch:** instead of a single rack dropdown, resolve which racks
  actually hold the selected stock items (each rack is token-bound; reuse
  `rack_info_list` + per-rack `locate_part`) and create/send a job to **every** rack
  that holds a needed part. Build metadata moves from one job to a job-per-rack map.
- This is the largest item; **split into two tickets (confirmed):**
  - **10a — stock-item selection:** per-part checkbox list when multiple stock items
    exist; plumb chosen `stock_id`(s) into the job item in `create_job_from_build`.
  - **10b — multi-rack fan-out:** resolve which racks hold the selected stock items and
    create/send a job to every rack that holds a needed part; Build metadata moves from
    one job to a job-per-rack map.

## 11. What happens if a BO line's text is too long?

Pick rows use `row_add_main_two_line()` (`widgets.cpp`) for name + metadata.

**Proposed solution:**
- Confirm the label uses LVGL long-mode `LV_LABEL_LONG_DOT` (ellipsis) or
  `LV_LABEL_LONG_SCROLL_CIRCULAR` with a fixed row width so long names don't overflow
  or wrap unboundedly. Set it centrally in `row_add_main_two_line` so every list screen
  inherits the fix.
- Verify on the longest realistic part name; prefer ellipsis for static legibility.

## 12. Order pick-job line items sequentially by slot number

`build_pick_active()` (`pick.cpp`) renders items in job order (BOM order).

**Proposed solution:**
- Sort the items by `located_slots` (lowest slot number) before rendering — ideally in
  the HMI render path so unlocated items (slot `-1`) sink to the bottom. Sorting client-
  side avoids a plugin change; if a stable server order is preferred, sort in
  `create_job_from_build`/`list_jobs` instead.

## 13. Pick-job "Picked" button is confusing — replace with a status that goes green when done

Each row has a "Picked"/"Done" toggle button (`on_picked()`) plus a stripe color.

**Proposed solution:**
- Remove the per-row button. Make picking flow from the physical action (reel removed
  from the lit slot, detected in `apply_slot_change`), and reflect status purely as a
  per-line indicator: blue (target/pending) → green check (picked) → red (error).
- Reuse the row stripe + a status glyph in the slot-number column. `on_picked()` logic
  (queue `JobPick`, darken LED) stays, just triggered by detection rather than a tap.

## 14. Quantity column on the pick-job screen doesn't matter — remove it

Rows show `row_add_qty_two_line()` (`pick.cpp`). Whole reels are picked, so the count
is noise.

**Proposed solution:**
- Drop the qty column from `build_pick_active()` rows. Frees horizontal space for the
  status indicator (item 13) and longer names (item 11).

## 15. Once fully picked, disable "Cancel job" — and clarify what Cancel does

`cancel_active_job()` reverts TARGET slots back to OCCUPIED and returns to the list;
`complete_active_job()` frees PICKED slots. Cancel stays available even at 100% picked.

**Proposed solution:**
- Once all items are picked, hide/disable "Cancel job" and show only "Complete job".
- Clarify semantics in UI copy: Cancel = "abandon this job, leave all reels in place"
  (un-targets slots, no inventory change); Complete = "finish, release picked slots."
  If a fully-picked job is canceled today, behavior is ambiguous — disabling it removes
  the footgun.

## 16. Pick from View page should pop up "remove the reel," then confirm and close

View's `on_pick()` arms the slot inline (lights blue, button flips to "Cancel") with no
modal; the confirmation is the button state.

**Proposed solution:**
- On pick tap, open a modal: "Remove the reel from slot #N" with the slot lit. Subscribe
  to the removal detection (`finish_pick_out`); when removal fires, swap the modal to a
  brief "Picked ✓" confirmation (+ green LED, item 4 pattern) and auto-close after ~1.5s.
- Provide a Cancel in the modal that calls `cancel_pick_out()`. Reuses the confirm modal.

## 17. Rename "scan part barcode" → "scan reel barcode" on load screen

The watching state title is "Scan part barcode" (`build_watching_state()`, `load.cpp`).

**Proposed solution:**
- One-line string change to "Scan reel barcode". Check the instruction subtitle wording
  for the same "part" → "reel" consistency.

## 18. Reconcile occupancy against physical buttons at boot (kill phantom parts)

At boot, `state_store::load()` applies saved/`InvenTree` occupancy onto the rack built
from live topology (`hw_mirror::sync_from_core`), but there's **no explicit check** for
"InvenTree/state says occupied, yet no physical reel button is pressed." Reported as
phantom occupied slots with nothing physically present.

**Proposed solution:**
- After `rebuild_rack()` + `state_store::load()` + first `GET /rack`, run a boot
  reconciliation pass: for each slot marked occupied (locally or server-side), check the
  physical input word from `hw_mirror`. If the button is *not* pressed, the slot is a
  phantom.
- Resolve phantoms by raising a reconcile prompt ("Slot #N marked occupied but empty —
  clear it?") and, on confirm, `queue_clear()` to drop the stale stock. Consider a
  conservative default (flag, don't auto-clear) to avoid deleting real stock during a
  transient bus read.
- This wants hw_mirror occupancy exposed per logical slot if it isn't already.

## 19. Load screen: make "place reel in any lit spot" the top line, much bigger

In the placed state, the target instruction "Place reel in any lit slot - X available"
is a mid-screen row (`build_placed_state()`, `load.cpp`).

**Proposed solution:**
- Promote it to the top of the placed view as a large, high-contrast heading (bump font
  to the largest theme size, bold). Demote the scanned-part card beneath it. It's the
  primary call-to-action, so it should dominate.

## 20. Show scan errors longer — persist recent errors

`set_status()` (`load.cpp`) writes the status line, but it clears as soon as the scanner
loses the code (next poll), so errors can flash by.

**Proposed solution:**
- Make scan errors sticky for a minimum dwell (~2s) before they can be overwritten by a
  "watching" status — track a `last_error_at` and suppress clearing until elapsed.
- Optionally add a small scrolling "recent errors" text view (last 3–5 messages with
  timestamps) below the status line for debugging bad scans. Pair with a beeper
  `error()` tone (item 5).

## 21. Will it accept a reel already loaded in a different SmartReel instance?

**Desired behaviour (clarified):** reject a reel currently housed in **this** instance
(you can't load the same reel here twice), but **accept** reels from any other SmartReel
— loading one transfers it over.

This already works with the existing resolve flow: `render_stock` reports `slot_num`
relative to the *requesting* rack (token-scoped), so `on_resolve_done` (`load.cpp`)
rejects when `slot_num > 0` (housed here) and proceeds to load otherwise (not in this
rack, including reels in a different SmartReel). No cross-rack block is added.

The companion behaviour — the *other* rack noticing its reel left when you load it here
— falls out of item 6's fast occupancy poll: the assign moves the stock item's location
in InvenTree, and the source rack detects the occupancy change within ~10s.

**Status:** no code change needed beyond the clarifying comment; the existing this-rack
rejection plus item 6 cover it.
