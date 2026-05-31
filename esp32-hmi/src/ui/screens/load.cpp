// LOAD -- three views:
//   Scan / watching : live QR poll; "Simulate scan" / "Cancel".
//   Scan / locked   : a valid code latched in; shows the part with
//                     "Rescan" / "Place reel" / "Cancel". Further scans
//                     are ignored until the user hits Rescan.
//   Placed          : clickable dot grid lights all empty slots.
//
// The QR scanner free-runs (it re-reports whatever code is in view), so
// we latch the first recognised scan and stop polling until a rescan.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "sensors/qr_scanner.h"

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace ui::screens {

using namespace theme;

namespace {
    constexpr int POLL_MS = 200;          // 5 Hz, well within device cap
    lv_timer_t* g_timer       = nullptr;
    lv_obj_t*   g_status_lbl  = nullptr;  // live status, only in watching view
    char        g_status_cache[96] = {};
}

// Set the watching-view status line, skipping the redraw if unchanged.
static void set_status(const char* text, bool warn) {
    if (!g_status_lbl) return;
    if (strcmp(g_status_cache, text) == 0) return;
    snprintf(g_status_cache, sizeof(g_status_cache), "%s", text);
    lv_label_set_text(g_status_lbl, text);
    lv_obj_set_style_text_color(g_status_lbl,
                                warn ? color::slot_warn() : color::text_muted(), 0);
}

// 5 Hz poll. No-ops unless the user is actively watching for a scan on
// the Load screen (current screen, Scan step, not yet locked).
static void on_poll(lv_timer_t*) {
    if (ui::current() != ui::Screen::Load) return;
    if (app::state().load_step != app::LoadStep::Scan) return;
    if (app::state().load_scan_locked) return;

    if (!qr_scanner::present()) {
        set_status("No scanner detected on I2C (0x0C). Simulate a scan below.", true);
        return;
    }

    char buf[256];
    if (qr_scanner::poll(buf, sizeof(buf))) {
        if (app::load_apply_scan(buf)) {
            ui::rebuild_current();        // -> locked view
            return;
        }
        // Recognised the scanner, not the code.
        char msg[96];
        snprintf(msg, sizeof(msg), "Unrecognised code: %.40s", buf);
        set_status(msg, true);
        return;
    }
    set_status("Watching... point the reel's QR code at the scanner.", false);
}

static void ensure_timer() {
    if (!g_timer) g_timer = lv_timer_create(on_poll, POLL_MS, nullptr);
}

// ---- Handlers ------------------------------------------------------
static void on_simulate(lv_event_t*) {
    app::mock_simulate_load_scan();
    ui::rebuild_current();
}
static void on_rescan(lv_event_t*) {
    // Drop any code the free-running sensor latched while we were
    // locked, so we don't instantly re-lock the same reel.
    char drain[256];
    (void)qr_scanner::poll(drain, sizeof(drain));
    app::load_rescan();
    ui::rebuild_current();
}
static void on_place(lv_event_t*) {
    app::load_begin_placement();
    ui::rebuild_current();
}
static void on_cancel(lv_event_t*) {
    app::mock_cancel_load();
    ui::go_back();
}
static void on_dot_pick(int slot_num) {
    app::mock_place_reel(slot_num);
    ui::navigate(Screen::Home);
}

// ---- Scan: watching view ------------------------------------------
static void build_watching_state(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_style_pad_gap(body, 14, 0);

    // Big QR placeholder box
    lv_obj_t* qr = lv_obj_create(body);
    lv_obj_remove_style_all(qr);
    lv_obj_set_size(qr, 96, 96);
    lv_obj_set_style_border_color(qr, color::text(), 0);
    lv_obj_set_style_border_width(qr, 4, 0);
    lv_obj_set_style_radius(qr, layout::RADIUS, 0);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* qr_l = lv_label_create(qr);
    lv_label_set_text(qr_l, "QR");
    lv_obj_set_style_text_font(qr_l, &lv_font_montserrat_14, 0);
    lv_obj_center(qr_l);

    lv_obj_t* prompt = lv_label_create(body);
    lv_label_set_text(prompt, "Scan part barcode");
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(prompt, color::text(), 0);

    // Live status line, driven by the poll timer.
    g_status_lbl = lv_label_create(body);
    lv_obj_set_style_text_font(g_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_status_lbl, color::text_muted(), 0);
    lv_obj_set_style_text_align(g_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_status_lbl, 480);
    lv_label_set_text(g_status_lbl,
                      "Point the scanner at the reel's QR code, or simulate a scan below.");
    g_status_cache[0] = 0;   // force the first timer paint

    lv_obj_t* btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    button(btn_row, "Simulate scan", BtnKind::Primary, on_simulate);
    button(btn_row, "Cancel",        BtnKind::Default, on_cancel);
}

// ---- Scan: locked view --------------------------------------------
static void build_locked_state(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_style_pad_gap(body, 12, 0);

    auto& part = app::state().load_part;

    lv_obj_t* res = card(body);
    lv_obj_set_width(res, LV_PCT(100));
    card_head(res, "SCANNED", LV_SYMBOL_OK);

    lv_obj_t* nm = lv_label_create(res);
    lv_label_set_text(nm, part.name);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(nm, color::text(), 0);

    char meta[80];
    snprintf(meta, sizeof(meta), "%s  %s  %s", part.id, part.pkg, part.mfg);
    lv_obj_t* mt = lv_label_create(res);
    lv_label_set_text(mt, meta);
    lv_obj_set_style_text_font(mt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(mt, color::text_muted(), 0);

    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, "Scan locked. Press Rescan to read a different reel.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, color::text_muted(), 0);

    lv_obj_t* btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    button(btn_row, "Place reel", BtnKind::Primary, on_place);
    button(btn_row, "Rescan",     BtnKind::Default, on_rescan);
    button(btn_row, "Cancel",     BtnKind::Default, on_cancel);
}

// ---- Placed view (slot grid) --------------------------------------
static void build_placed_state(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    auto& part = app::state().load_part;
    int lit = app::slots_target();

    // Scan-result card
    lv_obj_t* res = card(body);
    lv_obj_set_width(res, LV_PCT(100));

    lv_obj_t* nm = lv_label_create(res);
    lv_label_set_text(nm, part.name);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(nm, color::text(), 0);

    char meta[80];
    snprintf(meta, sizeof(meta), "%s  %s  %s", part.id, part.pkg, part.mfg);
    lv_obj_t* mt = lv_label_create(res);
    lv_label_set_text(mt, meta);
    lv_obj_set_style_text_font(mt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(mt, color::text_muted(), 0);

    // Target row inside card (small target dot + instruction)
    lv_obj_t* tgt = lv_obj_create(res);
    lv_obj_remove_style_all(tgt);
    lv_obj_set_size(tgt, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(tgt, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(tgt, color::border(), 0);
    lv_obj_set_style_border_width(tgt, 1, 0);
    lv_obj_set_style_pad_top(tgt, 10, 0);
    lv_obj_set_flex_flow(tgt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tgt, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(tgt, 10, 0);
    lv_obj_clear_flag(tgt, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* swatch = lv_obj_create(tgt);
    lv_obj_remove_style_all(swatch);
    lv_obj_add_style(swatch, const_cast<lv_style_t*>(&theme::s().dot_target), 0);
    lv_obj_set_size(swatch, 14, 14);
    lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);

    char hint[80];
    snprintf(hint, sizeof(hint),
             "Place reel in any lit slot - %d slot%s available",
             lit, lit == 1 ? "" : "s");
    lv_obj_t* tt = lv_label_create(tgt);
    lv_label_set_text(tt, hint);
    lv_obj_set_style_text_font(tt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tt, color::text(), 0);
    lv_obj_set_flex_grow(tt, 1);
    lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);

    // Clickable dot grid (only TARGET dots respond)
    DotGridOpts dg = {};
    dg.show_legend  = false;
    dg.header_label = "Available slots";
    dg.clickable    = true;
    dg.on_dot_click = on_dot_pick;
    dot_grid_card(body, dg);

    lv_obj_t* btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    button(btn_row, "Cancel", BtnKind::Default, on_cancel);
}

void build_load(lv_obj_t* body) {
    // g_status_lbl belongs to the watching view only; clear it so the
    // poll timer never touches a stale pointer after a rebuild.
    g_status_lbl = nullptr;

    if (app::state().load_step == app::LoadStep::Scan) {
        if (app::state().load_scan_locked) build_locked_state(body);
        else                               build_watching_state(body);
    } else {
        build_placed_state(body);
    }
    ensure_timer();
}

} // namespace ui::screens
