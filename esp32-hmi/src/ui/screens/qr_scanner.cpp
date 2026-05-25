// QR-SCANNER -- live view of the I2C-attached Useful Sensors Tiny Code
// Reader.
//
// Performance notes (history matters):
//   v1 used `lv_obj_clean` + recreate-all on each new scan. With
//   full_refresh LVGL and a 5 Hz poll, each LVGL invalidation triggered
//   a 768 KB write to PSRAM (back framebuffer), and the resulting
//   PSRAM contention starved the LCD DMA's bounce buffer -- visible
//   as tearing. This v2 avoids that by:
//
//     - Pre-creating LOG_MAX log rows once on screen build, then
//       updating their label text in place on each new scan.
//     - Only calling lv_label_set_text on the status label when the
//       displayed string actually changes (so the once-a-second
//       "Ns ago" tick is the only periodic invalidation).
//     - Skipping the I2C transaction + LVGL updates when the QR
//       screen isn't the current one. Scans that happen while the
//       user is away are not captured -- acceptable for a self-test
//       screen.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "sensors/qr_scanner.h"

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace ui::screens {

using namespace theme;

// Per-screen UI state. Persists for the lifetime of the cached
// screen so the scan count + log survive navigating away + back.
namespace {
    constexpr int   LOG_MAX = 8;
    constexpr int   POLL_MS = 200;       // 5 Hz, well within device cap

    lv_obj_t*   g_status_lbl   = nullptr;   // "x scans, last Ns ago"
    lv_obj_t*   g_last_lbl     = nullptr;   // big monospace last text
    lv_obj_t*   g_log_container= nullptr;   // contains the per-scan rows
    lv_obj_t*   g_log_rows[LOG_MAX]   = {}; // persistent row objects
    lv_obj_t*   g_log_labels[LOG_MAX] = {}; // text labels inside each row
    lv_timer_t* g_timer        = nullptr;

    char        g_log[LOG_MAX][96] = {};
    int         g_log_head    = 0;      // next slot to write
    int         g_log_count   = 0;
    int         g_scan_total  = 0;
    uint32_t    g_last_scan_ms = 0;

    // Cache of the last text painted into the status label. Lets us
    // skip the lv_label_set_text call (and the LVGL invalidation it
    // triggers) when the same string would be set.
    char        g_status_cache[96] = {};
}

static void push_log(const char* text) {
    snprintf(g_log[g_log_head], sizeof(g_log[g_log_head]), "%s", text);
    g_log_head = (g_log_head + 1) % LOG_MAX;
    if (g_log_count < LOG_MAX) g_log_count++;
}

// Build the LOG_MAX row objects exactly once. Hidden until used.
static void create_log_rows_once() {
    if (!g_log_container) return;
    for (int i = 0; i < LOG_MAX; ++i) {
        if (g_log_rows[i]) continue;

        lv_obj_t* row = lv_obj_create(g_log_container);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, color::border(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arrow, color::accent(), 0);

        lv_obj_t* t = lv_label_create(row);
        lv_label_set_text(t, "");
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(t, color::text(), 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
        lv_obj_set_flex_grow(t, 1);

        g_log_rows[i]   = row;
        g_log_labels[i] = t;
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
}

// "Empty state" label, shown only when g_log_count == 0.
static lv_obj_t* g_empty_lbl = nullptr;

static void update_log_display() {
    if (!g_log_container) return;
    // Empty state
    if (g_log_count == 0) {
        if (!g_empty_lbl) {
            g_empty_lbl = lv_label_create(g_log_container);
            lv_label_set_text(g_empty_lbl, "(no scans yet)");
            lv_obj_set_style_text_color(g_empty_lbl, color::text_muted(), 0);
            lv_obj_set_style_text_font(g_empty_lbl, &lv_font_montserrat_14, 0);
        }
        lv_obj_clear_flag(g_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    } else if (g_empty_lbl) {
        lv_obj_add_flag(g_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // Update each row label in place; hide unused rows.
    for (int i = 0; i < LOG_MAX; ++i) {
        if (!g_log_rows[i]) continue;
        if (i < g_log_count) {
            int log_idx = (g_log_head - 1 - i + LOG_MAX) % LOG_MAX;
            lv_label_set_text(g_log_labels[i], g_log[log_idx]);
            lv_obj_clear_flag(g_log_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_log_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Compose the status string, set the label only if the result has
// actually changed since last paint. That makes the once-per-200ms
// timer essentially free in steady state -- the integer seconds
// counter ticks over only once per real second.
static void update_status_label() {
    if (!g_status_lbl) return;

    char text[sizeof(g_status_cache)];
    bool warn = false;
    if (g_scan_total == 0) {
        if (qr_scanner::present()) {
            snprintf(text, sizeof(text), "Watching... point a QR code at the camera");
        } else {
            snprintf(text, sizeof(text),
                     "No scanner detected on I2C (0x0C). Re-probe from below.");
            warn = true;
        }
    } else {
        uint32_t age_s = (millis() - g_last_scan_ms) / 1000;
        snprintf(text, sizeof(text), "%d scan%s  last %lus ago",
                 g_scan_total, g_scan_total == 1 ? "" : "s",
                 (unsigned long)age_s);
    }

    if (strcmp(g_status_cache, text) == 0) return;   // no-op, no redraw
    snprintf(g_status_cache, sizeof(g_status_cache), "%s", text);
    lv_label_set_text(g_status_lbl, text);
    lv_obj_set_style_text_color(g_status_lbl,
                                warn ? color::slot_warn() : color::text_muted(), 0);
}

// The 5 Hz lv_timer. Bails out early if the user isn't currently on
// the QR screen so we don't pay any cost in the background.
static void on_poll(lv_timer_t* /*t*/) {
    if (ui::current() != ui::Screen::QrScanner) return;

    char buf[256];
    if (qr_scanner::poll(buf, sizeof(buf))) {
        g_scan_total++;
        g_last_scan_ms = millis();
        push_log(buf);
        if (g_last_lbl) lv_label_set_text(g_last_lbl, buf);
        update_log_display();
    }
    update_status_label();
}

static void on_clear(lv_event_t*) {
    g_log_count = 0;
    g_log_head  = 0;
    g_scan_total = 0;
    g_last_scan_ms = 0;
    g_status_cache[0] = 0;       // force the label to repaint
    if (g_last_lbl) lv_label_set_text(g_last_lbl, "(no scan yet)");
    update_status_label();
    update_log_display();
}

static void on_reprobe(lv_event_t*) {
    qr_scanner::init();
    g_status_cache[0] = 0;
    update_status_label();
}

void build_qr_scanner(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    // ---- Last-scan card ---------------------------------------------
    lv_obj_t* last_card = card(body);
    lv_obj_set_width(last_card, LV_PCT(100));
    card_head(last_card, "LAST SCAN", nullptr);

    g_last_lbl = lv_label_create(last_card);
    lv_label_set_text(g_last_lbl,
                      g_scan_total > 0 ? g_log[(g_log_head - 1 + LOG_MAX) % LOG_MAX]
                                       : "(no scan yet)");
    lv_obj_set_style_text_font(g_last_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_last_lbl, color::text(), 0);
    lv_label_set_long_mode(g_last_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_last_lbl, LV_PCT(100));

    g_status_lbl = lv_label_create(last_card);
    lv_obj_set_style_text_font(g_status_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(g_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_status_lbl, LV_PCT(100));
    g_status_cache[0] = 0;   // force first paint

    // ---- Recent-scans card ------------------------------------------
    lv_obj_t* log_card = card(body);
    lv_obj_set_width(log_card, LV_PCT(100));
    lv_obj_set_flex_grow(log_card, 1);
    card_head(log_card, "RECENT SCANS", nullptr);

    g_log_container = lv_obj_create(log_card);
    lv_obj_remove_style_all(g_log_container);
    lv_obj_set_size(g_log_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(g_log_container, 1);
    lv_obj_set_flex_flow(g_log_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_log_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_log_container, LV_SCROLLBAR_MODE_AUTO);

    // Wipe any stale row pointers (they belong to a previous instance
    // of the screen body if the user somehow forced a rebuild).
    for (int i = 0; i < LOG_MAX; ++i) {
        g_log_rows[i] = nullptr;
        g_log_labels[i] = nullptr;
    }
    g_empty_lbl = nullptr;

    create_log_rows_once();
    update_log_display();
    update_status_label();

    // ---- Buttons ----------------------------------------------------
    lv_obj_t* btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    button(btn_row, "Clear log",   BtnKind::Default, on_clear);
    button(btn_row, "Re-probe I2C", BtnKind::Default, on_reprobe);

    // ---- Polling timer ----------------------------------------------
    // Created once at first build. Polls at 5 Hz but no-ops when the
    // user is not on this screen (saves a 256-byte I2C transaction
    // + any LVGL touches per tick).
    if (!g_timer) {
        g_timer = lv_timer_create(on_poll, POLL_MS, nullptr);
    }
}

} // namespace ui::screens
