// QR-SCANNER -- live view of the I2C-attached Useful Sensors Tiny Code
// Reader. Polls the device at 5 Hz via an lv_timer (runs on the LVGL
// task, so shares the I2C bus with the touch driver naturally). Shows
// the most recent scan in big type at the top, with a short scrolling
// log of recent scans below. Used as a self-test that the scanner is
// wired + decoding correctly.
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
// screen (so the scan count + log survive navigating away + back).
namespace {
    constexpr int   LOG_MAX = 8;
    constexpr int   POLL_MS = 200;       // 5 Hz, well within device cap

    lv_obj_t*   g_status_lbl   = nullptr;   // "x scans, last Ns ago"
    lv_obj_t*   g_last_lbl     = nullptr;   // big monospace last text
    lv_obj_t*   g_log_container= nullptr;   // contains the per-scan rows
    lv_timer_t* g_timer        = nullptr;

    char        g_log[LOG_MAX][96] = {};
    int         g_log_head    = 0;      // next slot to write
    int         g_log_count   = 0;
    int         g_scan_total  = 0;
    uint32_t    g_last_scan_ms = 0;
}

static void push_log(const char* text) {
    snprintf(g_log[g_log_head], sizeof(g_log[g_log_head]), "%s", text);
    g_log_head = (g_log_head + 1) % LOG_MAX;
    if (g_log_count < LOG_MAX) g_log_count++;
}

static void rebuild_log_rows() {
    if (!g_log_container) return;
    lv_obj_clean(g_log_container);

    if (g_log_count == 0) {
        lv_obj_t* l = lv_label_create(g_log_container);
        lv_label_set_text(l, "(no scans yet)");
        lv_obj_set_style_text_color(l, color::text_muted(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        return;
    }
    // Show newest first.
    for (int i = 0; i < g_log_count; ++i) {
        int idx = (g_log_head - 1 - i + LOG_MAX) % LOG_MAX;
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
        lv_label_set_text(t, g_log[idx]);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(t, color::text(), 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
        lv_obj_set_flex_grow(t, 1);
    }
}

static void update_status_label() {
    if (!g_status_lbl) return;
    if (g_scan_total == 0) {
        if (qr_scanner::present()) {
            lv_label_set_text(g_status_lbl, "Watching...  point a QR code at the camera");
            lv_obj_set_style_text_color(g_status_lbl, color::text_muted(), 0);
        } else {
            lv_label_set_text(g_status_lbl, "No scanner detected on I2C (0x0C). Re-probe from Self Test.");
            lv_obj_set_style_text_color(g_status_lbl, color::slot_warn(), 0);
        }
        return;
    }
    char buf[64];
    uint32_t age_s = (millis() - g_last_scan_ms) / 1000;
    snprintf(buf, sizeof(buf), "%d scan%s  last %lus ago",
             g_scan_total, g_scan_total == 1 ? "" : "s",
             (unsigned long)age_s);
    lv_label_set_text(g_status_lbl, buf);
    lv_obj_set_style_text_color(g_status_lbl, color::text_muted(), 0);
}

static void on_poll(lv_timer_t* /*t*/) {
    char buf[256];
    if (qr_scanner::poll(buf, sizeof(buf))) {
        g_scan_total++;
        g_last_scan_ms = millis();
        push_log(buf);
        if (g_last_lbl) lv_label_set_text(g_last_lbl, buf);
        rebuild_log_rows();
    }
    // Always tick the "Ns ago" counter even if no fresh scan.
    update_status_label();
}

static void on_clear(lv_event_t*) {
    g_log_count = 0;
    g_log_head  = 0;
    g_scan_total = 0;
    g_last_scan_ms = 0;
    if (g_last_lbl) lv_label_set_text(g_last_lbl, "(no scan yet)");
    update_status_label();
    rebuild_log_rows();
}

static void on_reprobe(lv_event_t*) {
    qr_scanner::init();
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

    rebuild_log_rows();
    update_status_label();

    // ---- Buttons ----------------------------------------------------
    lv_obj_t* btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    button(btn_row, "Clear log", BtnKind::Default, on_clear);
    button(btn_row, "Re-probe I2C", BtnKind::Default, on_reprobe);

    // ---- Polling timer ---------------------------------------------
    // Created once (this is build_qr_scanner; the screen is cached
    // after first build). If the user navigates away the timer keeps
    // running so any scans during that time still land in the log --
    // 5 Hz I2C poll is trivial cost.
    if (!g_timer) {
        g_timer = lv_timer_create(on_poll, POLL_MS, nullptr);
    }
}

} // namespace ui::screens
