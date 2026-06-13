#include "ui/status_bar.h"
#include "ui/theme.h"
#include "ui/screen_manager.h"
#include "ui/anomaly_modal.h"

#include <stdio.h>

namespace ui {

static lv_obj_t* g_bar         = nullptr;
static lv_obj_t* g_back_btn    = nullptr;
static lv_obj_t* g_title       = nullptr;
static lv_obj_t* g_online_dot  = nullptr;
static lv_obj_t* g_online_lbl  = nullptr;
static lv_obj_t* g_badge_btn   = nullptr;
static lv_obj_t* g_badge_lbl   = nullptr;
static lv_obj_t* g_sd_pill     = nullptr;

static void on_back(lv_event_t*)  { ui::go_back(); }
static void on_badge(lv_event_t*) { ui::anomaly_modal_open_current(); }

void status_bar_init() {
    using namespace theme;

    g_bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_bar);
    lv_obj_add_style(g_bar, const_cast<lv_style_t*>(&theme::s().statusbar), 0);
    lv_obj_set_size(g_bar, layout::SCREEN_W, layout::STATUS_H);
    lv_obj_set_pos(g_bar, 0, 0);
    lv_obj_set_flex_flow(g_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_bar, LV_FLEX_ALIGN_START,
                                 LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Back button -----------------------------------------------------
    g_back_btn = lv_btn_create(g_bar);
    lv_obj_remove_style_all(g_back_btn);
    lv_obj_add_style(g_back_btn, const_cast<lv_style_t*>(&theme::s().statusbar_back_btn), 0);
    lv_obj_set_size(g_back_btn, 48, 48);   // 28pt arrow needs the room
    lv_obj_add_event_cb(g_back_btn, on_back, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_lbl = lv_label_create(g_back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    // Title -----------------------------------------------------------
    g_title = lv_label_create(g_bar);
    lv_label_set_text(g_title, "Reel Rack");
    lv_obj_set_style_text_align(g_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_title, color::text(), 0);
    lv_obj_set_flex_grow(g_title, 1);

    // Online pill -----------------------------------------------------
    lv_obj_t* pill = lv_obj_create(g_bar);
    lv_obj_remove_style_all(pill);
    lv_obj_add_style(pill, const_cast<lv_style_t*>(&theme::s().statusbar_pill), 0);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START,
                                 LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    g_online_dot = lv_obj_create(pill);
    lv_obj_remove_style_all(g_online_dot);
    lv_obj_set_size(g_online_dot, 10, 10);
    lv_obj_set_style_bg_color(g_online_dot, color::slot_picked(), 0);
    lv_obj_set_style_bg_opa(g_online_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_online_dot, LV_RADIUS_CIRCLE, 0);

    g_online_lbl = lv_label_create(pill);
    lv_label_set_text(g_online_lbl, "ONLINE");
    lv_obj_set_style_text_color(g_online_lbl, color::text_muted(), 0);
    lv_obj_set_style_text_font(g_online_lbl, &lv_font_montserrat_24, 0);

    // SD-missing pill (hidden when card is mounted) -------------------
    g_sd_pill = lv_obj_create(g_bar);
    lv_obj_remove_style_all(g_sd_pill);
    lv_obj_add_style(g_sd_pill, const_cast<lv_style_t*>(&theme::s().statusbar_pill), 0);
    lv_obj_set_size(g_sd_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g_sd_pill, color::slot_warn(), 0);
    lv_obj_set_style_border_color(g_sd_pill, color::slot_warn(), 0);
    lv_obj_clear_flag(g_sd_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* sd_lbl = lv_label_create(g_sd_pill);
    lv_label_set_text(sd_lbl, LV_SYMBOL_WARNING " NO SD - MOCK DATA");
    lv_obj_set_style_text_color(sd_lbl, color::text_on_accent(), 0);
    lv_obj_set_style_text_font(sd_lbl, &lv_font_montserrat_24, 0);
    lv_obj_add_flag(g_sd_pill, LV_OBJ_FLAG_HIDDEN);

    // Anomaly badge (hidden until count > 0) --------------------------
    g_badge_btn = lv_btn_create(g_bar);
    lv_obj_remove_style_all(g_badge_btn);
    lv_obj_add_style(g_badge_btn, const_cast<lv_style_t*>(&theme::s().statusbar_badge), 0);
    lv_obj_set_size(g_badge_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(g_badge_btn, on_badge, LV_EVENT_CLICKED, nullptr);
    g_badge_lbl = lv_label_create(g_badge_btn);
    lv_label_set_text(g_badge_lbl, "! 0");
    lv_obj_center(g_badge_lbl);
    lv_obj_add_flag(g_badge_btn, LV_OBJ_FLAG_HIDDEN);
}

void status_bar_set_sd_missing(bool missing) {
    if (!g_sd_pill) return;
    if (missing) lv_obj_clear_flag(g_sd_pill, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag (g_sd_pill, LV_OBJ_FLAG_HIDDEN);
}

void status_bar_set_title(const char* text) {
    if (g_title && text) lv_label_set_text(g_title, text);
}

void status_bar_set_back_visible(bool visible) {
    if (!g_back_btn) return;
    if (visible) lv_obj_clear_flag(g_back_btn, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag (g_back_btn, LV_OBJ_FLAG_HIDDEN);
}

void status_bar_set_online(bool online) {
    if (!g_online_dot) return;
    lv_obj_set_style_bg_color(g_online_dot,
        online ? theme::color::slot_picked() : theme::color::slot_error(), 0);
    lv_label_set_text(g_online_lbl, online ? "ONLINE" : "OFFLINE");
}

void status_bar_set_anomaly_count(int count) {
    if (!g_badge_btn) return;
    if (count <= 0) {
        lv_obj_add_flag(g_badge_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(g_badge_btn, LV_OBJ_FLAG_HIDDEN);
    char buf[16];
    snprintf(buf, sizeof(buf), "! %d", count);
    lv_label_set_text(g_badge_lbl, buf);
}

} // namespace ui
