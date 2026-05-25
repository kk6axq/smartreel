# Known bugs

Open issues that haven't been fixed yet. Update entries as fixes land
or as we learn more.

---

(No open issues at the moment.)

---

# Fixed

## ESP32-HMI: screen tearing after WiFi scan populates SSID list

Originally reported on the Configure -> Network screen after a WiFi
scan, then independently reproduced on the Pick Jobs screen near the
job-id labels and anywhere a scrolling list rebuilt. Manifested as
horizontal slices of the LCD shifting sideways for a few scanlines
during a refresh, worse while scrolling.

**Root cause (real one)**: LVGL was configured with two 40-line
partial draw buffers in PSRAM, and `flush_cb` was blitting those
partial regions into the active framebuffer via
`esp_lcd_panel_draw_bitmap`. The RGB DMA was simultaneously scanning
that same framebuffer out to the panel. Whenever LVGL's blit landed
on lines the DMA was about to read, the panel saw mid-update pixels
-- the visible "tear" was the seam between rendered-frame-N and
rendered-frame-N+1.

The IDF-5 bounce buffer fix we shipped earlier addressed a
*different* problem (PSRAM contention starving the DMA's refill
deadline). It didn't help here because the contention here is
*content* (LVGL writes to the same FB the panel is scanning), not
bandwidth.

**Fix (landed)**: switched display.cpp to the canonical Waveshare
"AVOID_TEARING_MODE 1" pattern: full-refresh LVGL + driver-managed
double FB + vsync-gated swap. The two framebuffers the IDF panel
driver allocates (num_fbs=2) are handed to LVGL directly as its
draw buffers; LVGL renders each frame entirely into the inactive
FB; flush_cb requests a buffer swap and blocks until the next
vsync fires (signalled by a semaphore given from the on_vsync ISR)
before letting LVGL reuse the just-departed FB. No partial writes
to the active FB ever happen, so tearing is structurally
impossible.

Confirmed gone on scroll + list rebuild + screen transition.

**Notes for the future**:
- We're now LVGL-bound by the panel refresh rate (~30-40 FPS at our
  16 MHz pclk / 800x480 timings). For an HMI this is plenty; LVGL
  will only redraw when something is dirty.
- If we ever want partial-area updates back for cheaper animation,
  switch to AVOID_TEARING_MODE 3 (direct-mode + dirty-area copy
  between the two FBs). Substantially more complex; not worth it
  unless we see actual frame-time problems.
- The original `lv_obj_clean()` + rebuild churn on Pick Active is no
  longer visible. We can keep the always-dirty marker on
  ConfigNetwork without paying for it.

**Filed**: 2026-05-18.   **Fixed**: 2026-05-19.
