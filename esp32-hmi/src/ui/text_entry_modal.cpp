#include "ui/text_entry_modal.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "ui/screen_manager.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace ui {

using namespace theme;

namespace {
    lv_obj_t*       g_root      = nullptr;
    lv_obj_t*       g_title_lbl = nullptr;
    lv_obj_t*       g_ta        = nullptr;
    lv_obj_t*       g_kb        = nullptr;
    lv_obj_t*       g_show_btn  = nullptr;
    TextEntrySaveFn g_on_save   = nullptr;
    bool            g_pw_mode   = false;
}

// ---- handlers ------------------------------------------------------

static void on_cancel(lv_event_t*) {
    text_entry_modal_close();
}

static void on_save(lv_event_t*) {
    const char* v = lv_textarea_get_text(g_ta);
    TextEntrySaveFn cb = g_on_save;       // copy before close clears it
    text_entry_modal_close();
    if (cb) cb(v ? v : "");
    // The save callback may want a redraw -- it's responsible for
    // calling ui::rebuild_current() if so. We don't do it here because
    // some callbacks navigate elsewhere.
}

static void on_show_toggle(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    bool pw = lv_textarea_get_password_mode(g_ta);
    lv_textarea_set_password_mode(g_ta, !pw);
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_label_set_text(lbl, !pw ? "Hide" : "Show");
}

static void on_kb_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)       on_save(e);
    else if (code == LV_EVENT_CANCEL) on_cancel(e);
}

// ---- build ---------------------------------------------------------

void text_entry_modal_init() {
    g_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, layout::SCREEN_W, layout::SCREEN_H);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, color::bg(), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE);

    // Title bar
    lv_obj_t* tbar = lv_obj_create(g_root);
    lv_obj_remove_style_all(tbar);
    lv_obj_set_size(tbar, layout::SCREEN_W, layout::STATUS_H);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, color::surface(), 0);
    lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(tbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(tbar, color::border(), 0);
    lv_obj_set_style_border_width(tbar, 1, 0);
    lv_obj_set_style_pad_hor(tbar, 12, 0);
    lv_obj_set_flex_flow(tbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tbar, LV_FLEX_ALIGN_START,
                                 LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    g_title_lbl = lv_label_create(tbar);
    lv_label_set_text(g_title_lbl, "");
    lv_obj_set_style_text_font(g_title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_title_lbl, color::text(), 0);
    lv_obj_set_flex_grow(g_title_lbl, 1);

    lv_obj_t* close_btn = lv_btn_create(tbar);
    lv_obj_remove_style_all(close_btn);
    lv_obj_set_size(close_btn, 32, 32);
    lv_obj_set_style_bg_color(close_btn, color::surface(), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(close_btn, color::border_strong(), 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_radius(close_btn, layout::RADIUS, 0);
    lv_obj_add_event_cb(close_btn, on_cancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cl = lv_label_create(close_btn);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(cl, color::text(), 0);
    lv_obj_center(cl);

    // Body
    lv_obj_t* body = lv_obj_create(g_root);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, layout::STATUS_H);
    lv_obj_set_size(body, layout::SCREEN_W, layout::SCREEN_H - layout::STATUS_H);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_gap(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // Input row
    lv_obj_t* irow = lv_obj_create(body);
    lv_obj_remove_style_all(irow);
    lv_obj_set_size(irow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(irow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(irow, LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(irow, 8, 0);
    lv_obj_clear_flag(irow, LV_OBJ_FLAG_SCROLLABLE);

    g_ta = lv_textarea_create(irow);
    lv_textarea_set_one_line(g_ta, true);
    lv_obj_set_flex_grow(g_ta, 1);
    lv_obj_set_style_text_font(g_ta, &lv_font_montserrat_24, 0);
    lv_obj_set_style_border_color(g_ta, color::border_strong(), 0);
    lv_obj_set_style_border_width(g_ta, 1, 0);
    lv_obj_set_style_radius(g_ta, layout::RADIUS, 0);
    lv_obj_set_style_bg_color(g_ta, color::surface(), 0);

    g_show_btn = button(irow, "Show", BtnKind::Default, on_show_toggle);

    // Actions
    lv_obj_t* actions = lv_obj_create(body);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END,
                                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(actions, 8, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    button(actions, "Cancel", BtnKind::Default, on_cancel);
    button(actions, "Save",   BtnKind::Primary, on_save);

    // Keyboard
    g_kb = lv_keyboard_create(body);
    lv_obj_set_size(g_kb, LV_PCT(100), 240);
    lv_keyboard_set_textarea(g_kb, g_ta);
    lv_obj_add_event_cb(g_kb, on_kb_event, LV_EVENT_ALL, nullptr);
}

void text_entry_modal_open(const TextEntryOpts& opts) {
    if (!g_root) return;
    g_on_save = opts.on_save;
    g_pw_mode = opts.password_mode;

    lv_label_set_text(g_title_lbl, opts.title ? opts.title : "Edit");

    lv_textarea_set_text(g_ta, opts.initial ? opts.initial : "");
    lv_textarea_set_placeholder_text(g_ta, opts.placeholder ? opts.placeholder : "");
    lv_textarea_set_password_mode(g_ta, opts.password_mode);
    if (opts.max_len > 0) lv_textarea_set_max_length(g_ta, opts.max_len);
    else                  lv_textarea_set_max_length(g_ta, 0);

    // Show/Hide button only matters when masking input.
    if (g_show_btn) {
        if (opts.password_mode) lv_obj_clear_flag(g_show_btn, LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_add_flag  (g_show_btn, LV_OBJ_FLAG_HIDDEN);
        // Reset show-btn label to "Show"
        lv_obj_t* lbl = lv_obj_get_child(g_show_btn, 0);
        if (lbl) lv_label_set_text(lbl, "Show");
    }

    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
}

void text_entry_modal_close() {
    if (!g_root) return;
    g_on_save = nullptr;
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
}

} // namespace ui
