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

---

# Round 2 — post-implementation review (2026-06-16)

Feedback after running the Round 1 build on hardware. Several items are regressions
in the Round 1 changes. Each heading is one issue; the proposal references the real
code path.

## R1. Pick "Refresh" shows a weird empty rounded rectangle (and R4: same after a load)

The new toast (`notify.cpp`) renders as an empty rounded box because the label is sized
`LV_PCT(100)` inside a `LV_SIZE_CONTENT` box — percent-of-content resolves to ~0 width,
so the text is invisible and only the bordered rectangle shows. Same root cause for the
post-load "Loaded to slot #N" toast (R4).

**Proposed solution:**
- In `notify_init()`, stop forcing the toast label to `LV_PCT(100)`. Let the label
  size to its text (`LV_SIZE_CONTENT`) and cap the *box* width (`max_width`), so the
  box hugs the text. Only switch the label to a fixed width + wrap when the text
  actually exceeds the cap.
- Verify the toast shows its text on both the pick-refresh and load paths; consider a
  small leading icon (ℹ/✓) so it reads as a notification, not a stray rectangle.

## R2. Scan-reel screen: add a big left-pointing arrow toward the scanner

The scanner is physically on the **left** of the unit; the watching view (`load.cpp`
`build_watching_state`) gives no directional cue.

**Proposed solution:**
- Add a large left-pointing arrow (`LV_SYMBOL_LEFT` at a big font, or a drawn
  triangle) beside the "Scan reel barcode" prompt, pointing at the scanner. Keep it
  only on the watching view, not the placed view.

## R3. Settings screen tears the display (low priority)

Noted as low priority. Tearing means rendering work is hitting `PRO_CPU` (the RGB LCD
DMA), per the CPU-pinning rule. The Settings grid build is heavier than other screens.

**Proposed solution (deferred):**
- Profile what runs on `PRO_CPU` while Settings builds; ensure no blocking/allocation
  happens off `APP_CPU`. Likely the large menu-grid build or a synchronous read.
  Tracked but not prioritized.

## R5. Rack overview collapses a whole port into one giant slot until a divider is toggled

`effective_topology()` (`app_state.cpp`) derives dividers from the live input bits when
the rack isn't commissioned: **absent bit ⇒ divider pulled**. At boot the divider bits
read all-absent (the Core hasn't latched them, or they aren't valid yet), so every
slot in a port combines into one. The first real divider event refreshes the bits and
`rebuild_rack()` then shows reality.

**Proposed solution:**
- Root cause: make `hw_mirror::sync_from_core()` get a *valid* divider reading at boot
  — re-read `read_inputs` after a short settle, or have the Core latch divider state
  before the HMI reads. Until a confirmed reading, **default dividers to PRESENT**
  (standard 1-wide slots) instead of pulled, so a port never collapses into one slot.
- Safer still: only derive numbering from live bits pre-commission; once commissioned,
  use the committed layout (already supported) so a flaky boot read can't reshape it.

## R6. Going back from Rack overview shows an empty rack despite a just-placed part

Linked to R5. `rebuild_rack()` preserves slot contents **by logical number**. When the
divider state corrects itself (R5), the logical numbering changes (giant slot → real
slots), so the placed part's old number no longer matches any new slot and its contents
are dropped — the rack looks empty.

**Proposed solution:**
- Fix R5 so numbering is correct from boot (removes the re-number entirely).
- Defensively, remap preserved contents by **physical anchor** (`port, module, mslot`)
  rather than logical number in `rebuild_rack()`, so contents survive a re-numbering.

## R7. Divider edits on an occupied (multi-wide) slot must be illegal

Adding a divider *into* a multi-wide slot, or removing a divider *adjacent to* an
occupied slot, changes the slot's width while a reel is loaded across that full width —
it can't be re-shaped without unloading first. Today `handle_divider_change`
(`main.cpp`) only checks the committed layout for mismatch; it doesn't guard occupancy.

**Proposed solution:**
- In `handle_divider_change` (committed path) and the pre-commission rebuild, before
  applying a divider change, check whether the spanned/adjacent logical slot is
  `OCCUPIED` (part valid). If so, reject the change: raise an anomaly ("unload the reel
  in slot #N before changing this divider") and do **not** re-shape the rack. Restore
  the expected divider state in the UI.

## R8. Home page should be a 2×2 grid (Load/Pick top, View/Settings bottom)

Round 1 left Home as three tiles + a full-width Settings. The requested layout is 2×2.

**Proposed solution:**
- In `build_home` (`home.cpp`), switch to a 2-col × 2-row grid:
  `Load (0,0)  Pick (1,0)  /  View (0,1)  Settings (1,1)`.

## R9. "Rack overview" button on the View screen should be full-width and bold

Round 1 added a small right-aligned button (`view.cpp`).

**Proposed solution:**
- Make the Rack-overview button span the full row width (`LV_PCT(100)`) and use a
  bold/large label so it reads as the primary drill-in.

## R10. "Reel removed" modal: the Action detail line overlaps

The anomaly modal detail rows (`anomaly_modal.cpp` `render_from_state`) are flex rows
with the key left and value right (`SPACE_BETWEEN`). A long value like
"Replace the reel, or unload it" collides with the key.

**Proposed solution:**
- Give the value label `flex_grow` with right align + `LV_LABEL_LONG_DOT` (ellipsis),
  or wrap it onto its own line, so it can't overlap the key. Also shorten the Action
  text (e.g. "Replace or unload"). Apply to all detail rows, not just this one.

## R11. Replacing an illegally-removed reel wrongly raises a warning (with overlapping text)

After an illegal removal (red "removed" modal), re-inserting the reel hits the
"unexpected insertion" branch in `apply_slot_change` (`main.cpp`), which raises the
"Added" warning. It shouldn't — putting the reel back is the *resolution* of the
removal, not a new anomaly. The warning modal's detail lines also overlap (same layout
bug as R10).

**Proposed solution:**
- In `apply_slot_change`, when a reel is inserted into a slot currently in `ERROR`
  (illegally-removed) state, treat it as "reel replaced": restore the slot to
  `OCCUPIED` (the part is still valid), clear the removal anomaly, stop the red flash,
  and do **not** raise an "Added" warning.
- Fix the detail-row overlap via R10.

## R12. Replacing an illegally-removed reel drops it from the View screen

The illegal-removal path sets the slot to `ERROR` but keeps `part.valid`. `build_view`
only lists `OCCUPIED && part.valid` slots, so an `ERROR` (or `WARN`) slot with a valid
part vanishes from View. R11's fix (restore to `OCCUPIED` on replace) makes it reappear.

**Proposed solution:**
- Primary: R11's "reel replaced → OCCUPIED" restore brings it back into View.
- Optionally also surface `ERROR`/`WARN` slots in View (with a state chip) so a reel
  mid-anomaly is never invisible.

## R13. View rows: add column headers or label the slot as "Slot 40"

`row_add_slot_num` shows "#40", which reads as a count/ID, not a slot.

**Proposed solution:**
- Change the View row tag from "#40" to "Slot 40" (or add a header row:
  Slot / Part / Qty). Simplest is the "Slot N" relabel in `build_view`'s tag.

## R14. Remove the "Exit" button from the Settings menu

`build_configure` (`configure.cpp`) has an Exit tile; the status-bar back button
already returns Home, so it's redundant.

**Proposed solution:**
- Drop the Exit menu item (and `on_exit`). Re-flow the menu grid (5 items) — e.g.
  Slots/Network/Self Test on row 0, Firmware/Dividers on row 1.

## R15. Purpose of the Slot-config "Numbering" section + "Save numbering" button

These are **non-functional mockup leftovers**. In `config_slots.cpp` the "Order" and
"Skip numbers" fields are `form_input(...)` — display-only labels that take no input —
and the "Save numbering" button is created with **no callback**. Numbering is now
derived automatically from the live topology (`SlotMap`), so the section does nothing.
The "Rack identity" Rack-name / location inputs are the same display-only mockups.

**Proposed solution:**
- Remove the NUMBERING card (and the non-functional RACK IDENTITY inputs), or, if a
  skip-list is genuinely wanted, implement it: persist a skip set in `config_store`,
  apply it in the numbering engine, and wire the Save button. Recommend removing the
  dead UI for now.

## R16. Self Test "Single slot" → Light button does nothing

`on_leds_single` calls `leds::light_slot(s_single_slot, ...)`, which resolves
`slot_by_num(s_single_slot)`; if that number isn't a present logical slot it returns
`BufferTooSmall` and nothing lights — silently. The default `s_single_slot = 12` won't
exist while the rack is mis-combined (R5: a port collapsed into a few giant slots
numbered 1, 17, …), so "Light" no-ops.

**Proposed solution:**
- Validate the slot before lighting: if `slot_by_num` is null, show feedback ("slot N
  not present") instead of failing silently, and default `s_single_slot` to the first
  present logical slot. Fixing R5 also restores the normal 1..N numbering so 12 is
  valid again.

