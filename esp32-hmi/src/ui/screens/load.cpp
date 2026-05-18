// LOAD -- two states:
//   Scan   : waiting on QR; "Simulate scan" / "Cancel"
//   Placed : part identified; clickable dot grid lights all empty slots.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"

#include <stdio.h>

namespace ui::screens {

using namespace theme;

static void on_simulate(lv_event_t*) {
    app::mock_simulate_load_scan();
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

static void build_scan_state(lv_obj_t* body) {
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

    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, "Point the scanner at the reel's QR code, or simulate a scan below.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, color::text_muted(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, 480);

    lv_obj_t* btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    button(btn_row, "Simulate scan", BtnKind::Primary, on_simulate);
    button(btn_row, "Cancel",        BtnKind::Default, on_cancel);
}

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
    if (app::state().load_step == app::LoadStep::Scan) {
        build_scan_state(body);
    } else {
        build_placed_state(body);
    }
}

} // namespace ui::screens
