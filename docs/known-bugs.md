# Known bugs

Open issues that haven't been fixed yet. Update entries as fixes land
or as we learn more.

---

## ESP32-HMI: screen tearing after WiFi scan populates SSID list

**Symptoms**
- Triggered the first time the user opens Configure -> Network and
  taps Scan, or whenever the network list (re)populates.
- Visible artifact: horizontal slices of the LCD shift sideways for
  a few scanlines, similar to the pre-IDF-5 PSRAM-contention "wobble"
  but localised to specific lines instead of a global jitter.
- Tearing **persists** after the scan completes and even after
  connecting to a network.
- Gets noticeably worse while scrolling the form (vertical drag).

**Why it probably happens**
The IDF-5 RGB driver's bounce-buffer fix the main wobble case (LVGL
allocator bursts vs LCD DMA refill deadline). The scan-list path
seems to introduce a *different* contention pattern that the bounce
buffer doesn't fully cover:

1. `WiFi.scanNetworks()` blocks for ~3 s while ESP32's WiFi task does
   heavy PSRAM allocation for scan-result buffers + 802.11 RX queues.
2. When the scan returns, the network screen builds 10+ list rows
   in a single LVGL refresh tick. Each row is a flex container with
   3-4 labels + an event-cb context heap-alloc. That's a burst of
   small PSRAM allocs on the LVGL task.
3. Scrolling triggers LVGL to re-lay-out all visible children every
   frame (the row container has `LV_DIR_VER` scrolling). Each
   relayout walks the child list and reads style data from PSRAM.

The "always-dirty" treatment we apply to the Network screen means
each entry/exit rebuilds the whole list, which keeps the alloc
churn going long after the scan itself completes.

**Hypotheses to investigate** (in rough order of likely impact)

1. **Pre-allocate the row context structs.** `RowCtx` is `new`'d
   per row in `config_network.cpp` and freed on the row's
   `LV_EVENT_DELETE`. Use a fixed pool of `SCAN_MAX` contexts
   instead.
2. **Drop the "always-dirty" mark for ConfigNetwork.** Switch to
   dirty-on-explicit-events (after scan_run, after set_credentials)
   instead of dirty-on-every-entry. Re-entering should pull cached
   state from `wifi_mgr` without rebuilding the list.
3. **Cap the visible list height** so the scrollable inner container
   has a fixed size, eliminating LVGL re-layout of children that
   aren't on screen.
4. **Move the scan onto an async task** (`WiFi.scanNetworks(true)`)
   and poll for completion from the LVGL task so the heavy WiFi
   alloc burst doesn't happen synchronously with the user tap.
5. If 1-4 don't help, look at LVGL's `LV_DRAW_BUF` sizing. We're
   using 40-line LVGL line buffers; a larger refresh region might
   reduce the number of partial draws during scroll.

**Workarounds**
- None: the artifact is cosmetic, doesn't affect functionality.
- Avoid scrolling on the Network screen if you're staring at it.

**Filed**: 2026-05-18
