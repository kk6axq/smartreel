#include "ui/wifi_password_modal.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "ui/screen_manager.h"
#include "net/wifi_mgr.h"

#include <stdio.h>
#include <string.h>

namespace ui {

using namespace theme;

static lv_obj_t* g_root      = nullptr;
static lv_obj_t* g_title_lbl = nullptr;
static lv_obj_t* g_pw_ta     = nullptr;
static lv_obj_t* g_kb        = nullptr;
static char      g_ssid[33]  = "";
static bool      g_open_net  = false;

// ---- Event handlers -------------------------------------------------
static void on_cancel(lv_event_t*) {
    wifi_password_modal_close();
}

static void on_connect(lv_event_t*) {
    const char* pw = lv_textarea_get_text(g_pw_ta);
    wifi_mgr::set_credentials(g_ssid, pw ? pw : "");
    wifi_password_modal_close();
    // Refresh the Network screen to show the new state.
    ui::rebuild_current();
}

static void on_show_toggle(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    bool pw_mode = lv_textarea_get_password_mode(g_pw_ta);
    lv_textarea_set_password_mode(g_pw_ta, !pw_mode);
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_label_set_text(lbl, !pw_mode ? "Hide" : "Show");
}

// LVGL keyboard fires LV_EVENT_READY when the user taps the green
// checkmark, and LV_EVENT_CANCEL on the X. Treat them as Connect /
// Cancel so the keyboard's own buttons work too.
static void on_kb_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)       on_connect(e);
    else if (code == LV_EVENT_CANCEL) on_cancel(e);
}

// ---- Build ----------------------------------------------------------
void wifi_password_modal_init() {
    g_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, layout::SCREEN_W, layout::SCREEN_H);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, color::bg(), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    // Eat clicks behind the modal.
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
    lv_label_set_text(g_title_lbl, "Connect to network");
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

    // Body: password row + actions, then keyboard fills the rest.
    lv_obj_t* body = lv_obj_create(g_root);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, layout::STATUS_H);
    lv_obj_set_size(body, layout::SCREEN_W, layout::SCREEN_H - layout::STATUS_H);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_gap(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // Password row
    lv_obj_t* pwrow = lv_obj_create(body);
    lv_obj_remove_style_all(pwrow);
    lv_obj_set_size(pwrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pwrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pwrow, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(pwrow, 8, 0);
    lv_obj_clear_flag(pwrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(pwrow);
    lv_label_set_text(lbl, "Password");
    lv_obj_set_style_text_color(lbl, color::text(), 0);
    lv_obj_set_style_min_width(lbl, 88, 0);

    g_pw_ta = lv_textarea_create(pwrow);
    lv_textarea_set_one_line(g_pw_ta, true);
    lv_textarea_set_password_mode(g_pw_ta, true);
    lv_textarea_set_placeholder_text(g_pw_ta, "<enter password>");
    lv_obj_set_flex_grow(g_pw_ta, 1);
    lv_obj_set_style_text_font(g_pw_ta, &lv_font_montserrat_24, 0);
    lv_obj_set_style_border_color(g_pw_ta, color::border_strong(), 0);
    lv_obj_set_style_border_width(g_pw_ta, 1, 0);
    lv_obj_set_style_radius(g_pw_ta, layout::RADIUS, 0);
    lv_obj_set_style_bg_color(g_pw_ta, color::surface(), 0);

    // Show / hide toggle
    lv_obj_t* show_btn = button(pwrow, "Show", BtnKind::Default, on_show_toggle);
    (void)show_btn;

    // Action row (Cancel / Connect)
    lv_obj_t* actions = lv_obj_create(body);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END,
                                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(actions, 8, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    button(actions, "Cancel",  BtnKind::Default, on_cancel);
    button(actions, "Connect", BtnKind::Primary, on_connect);

    // Keyboard - fills the rest of the body.
    g_kb = lv_keyboard_create(body);
    lv_obj_set_size(g_kb, LV_PCT(100), 240);
    lv_keyboard_set_textarea(g_kb, g_pw_ta);
    lv_obj_add_event_cb(g_kb, on_kb_event, LV_EVENT_ALL, nullptr);
}

void wifi_password_modal_open(const char* ssid, bool open_network) {
    if (!g_root) return;
    snprintf(g_ssid, sizeof(g_ssid), "%s", ssid ? ssid : "");
    g_open_net = open_network;

    char buf[64];
    snprintf(buf, sizeof(buf), "Connect to \"%s\"", g_ssid);
    lv_label_set_text(g_title_lbl, buf);

    // Wipe the password field on every open.
    lv_textarea_set_text(g_pw_ta, "");
    lv_textarea_set_password_mode(g_pw_ta, true);

    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
}

void wifi_password_modal_close() {
    if (!g_root) return;
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
}

} // namespace ui
