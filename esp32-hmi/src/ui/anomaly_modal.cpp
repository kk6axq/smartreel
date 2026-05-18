#include "ui/anomaly_modal.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "ui/status_bar.h"

#include <stdio.h>

namespace ui {

using namespace theme;

static lv_obj_t* g_backdrop  = nullptr;
static lv_obj_t* g_box       = nullptr;
static lv_obj_t* g_header    = nullptr;
static lv_obj_t* g_title     = nullptr;
static lv_obj_t* g_body      = nullptr;
static lv_obj_t* g_msg       = nullptr;
static lv_obj_t* g_detail    = nullptr;

static void on_dismiss(lv_event_t*)   { anomaly_modal_dismiss(); }
static void on_resolve(lv_event_t*)   { anomaly_modal_resolve(); }
static void on_backdrop(lv_event_t*)  { /* tap outside: keep open */ }

void anomaly_modal_init() {
    // Backdrop fills the whole 800x480 with semi-transparent black.
    // It's the parent for the modal box. Hidden by default.
    g_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_backdrop);
    lv_obj_set_size(g_backdrop, layout::SCREEN_W, layout::SCREEN_H);
    lv_obj_set_pos(g_backdrop, 0, 0);
    lv_obj_set_style_bg_color(g_backdrop, color::backdrop(), 0);
    lv_obj_set_style_bg_opa(g_backdrop, color::backdrop_opa, 0);
    lv_obj_set_flex_flow(g_backdrop, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_backdrop, LV_FLEX_ALIGN_CENTER,
                                       LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_backdrop, on_backdrop, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(g_backdrop, LV_OBJ_FLAG_HIDDEN);

    // Modal box ----------------------------------------------------
    g_box = lv_obj_create(g_backdrop);
    lv_obj_remove_style_all(g_box);
    lv_obj_add_style(g_box, const_cast<lv_style_t*>(&theme::s().modal_box), 0);
    lv_obj_set_size(g_box, 480, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(g_box, LV_OBJ_FLAG_SCROLLABLE);
    // Eat clicks so they don't propagate to the backdrop click handler.
    lv_obj_add_flag(g_box, LV_OBJ_FLAG_CLICKABLE);

    // Header
    g_header = lv_obj_create(g_box);
    lv_obj_remove_style_all(g_header);
    lv_obj_add_style(g_header, const_cast<lv_style_t*>(&theme::s().modal_header_error), 0);
    lv_obj_set_size(g_header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_header, LV_FLEX_ALIGN_START,
                                     LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon_circle = lv_obj_create(g_header);
    lv_obj_remove_style_all(icon_circle);
    lv_obj_set_size(icon_circle, 24, 24);
    lv_obj_set_style_radius(icon_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(icon_circle, lv_color_white(), 0);
    lv_obj_set_style_border_width(icon_circle, 2, 0);
    lv_obj_set_style_bg_opa(icon_circle, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(icon_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* bang = lv_label_create(icon_circle);
    lv_label_set_text(bang, "!");
    lv_obj_set_style_text_color(bang, lv_color_white(), 0);
    lv_obj_set_style_text_font(bang, &lv_font_montserrat_14, 0);
    lv_obj_center(bang);

    g_title = lv_label_create(g_header);
    lv_label_set_text(g_title, "Anomaly");
    lv_obj_set_style_text_color(g_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_title, &lv_font_montserrat_14, 0);
    lv_obj_set_flex_grow(g_title, 1);

    // Body
    g_body = lv_obj_create(g_box);
    lv_obj_remove_style_all(g_body);
    lv_obj_set_size(g_body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(g_body, 16, 0);
    lv_obj_set_style_pad_ver(g_body, 14, 0);
    lv_obj_set_style_pad_gap(g_body, 10, 0);
    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(g_body, LV_OBJ_FLAG_SCROLLABLE);

    g_msg = lv_label_create(g_body);
    lv_label_set_text(g_msg, "");
    lv_obj_set_style_text_font(g_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_msg, color::text(), 0);
    lv_label_set_long_mode(g_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_msg, LV_PCT(100));

    g_detail = lv_obj_create(g_body);
    lv_obj_remove_style_all(g_detail);
    lv_obj_set_size(g_detail, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g_detail, color::bg(), 0);
    lv_obj_set_style_bg_opa(g_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_detail, color::border(), 0);
    lv_obj_set_style_border_width(g_detail, 1, 0);
    lv_obj_set_style_radius(g_detail, layout::RADIUS, 0);
    lv_obj_set_style_pad_all(g_detail, 10, 0);
    lv_obj_set_style_pad_gap(g_detail, 2, 0);
    lv_obj_set_flex_flow(g_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(g_detail, LV_OBJ_FLAG_SCROLLABLE);

    // Actions
    lv_obj_t* actions = lv_obj_create(g_box);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(actions, 16, 0);
    lv_obj_set_style_pad_bottom(actions, 14, 0);
    lv_obj_set_style_pad_gap(actions, 8, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* dismiss = button(actions, "Dismiss", BtnKind::Default,  on_dismiss);
    lv_obj_set_flex_grow(dismiss, 1);
    lv_obj_t* resolve = button(actions, "Mark resolved", BtnKind::Primary, on_resolve);
    lv_obj_set_flex_grow(resolve, 1);
}

static void apply_mood(app::AnomalyMood m) {
    // Reset to error styles, then add overlays.
    lv_obj_remove_style(g_box,    const_cast<lv_style_t*>(&theme::s().modal_box_warn), 0);
    lv_obj_remove_style(g_box,    const_cast<lv_style_t*>(&theme::s().modal_box_info), 0);
    lv_obj_remove_style(g_header, const_cast<lv_style_t*>(&theme::s().modal_header_warn), 0);
    lv_obj_remove_style(g_header, const_cast<lv_style_t*>(&theme::s().modal_header_info), 0);

    if (m == app::AnomalyMood::Warn) {
        lv_obj_add_style(g_box,    const_cast<lv_style_t*>(&theme::s().modal_box_warn),  0);
        lv_obj_add_style(g_header, const_cast<lv_style_t*>(&theme::s().modal_header_warn), 0);
    } else if (m == app::AnomalyMood::Info) {
        lv_obj_add_style(g_box,    const_cast<lv_style_t*>(&theme::s().modal_box_info),  0);
        lv_obj_add_style(g_header, const_cast<lv_style_t*>(&theme::s().modal_header_info), 0);
    }
    // Force re-styling.
    lv_obj_invalidate(g_box);
}

static void render_from_state() {
    auto& a = app::state().anomaly;
    if (a.kind == app::AnomalyKind::None) return;
    apply_mood(a.mood);
    lv_label_set_text(g_title, a.title);
    lv_label_set_text(g_msg,   a.message);

    // Re-build detail rows.
    lv_obj_clean(g_detail);
    for (int i = 0; i < a.n_detail; ++i) {
        lv_obj_t* row = lv_obj_create(g_detail);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* k = lv_label_create(row);
        lv_label_set_text(k, a.detail[i].k);
        lv_obj_set_style_text_color(k, color::text_muted(), 0);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_12, 0);

        lv_obj_t* v = lv_label_create(row);
        lv_label_set_text(v, a.detail[i].v);
        lv_obj_set_style_text_color(v, color::text(), 0);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_12, 0);
    }
}

void anomaly_modal_open_current() {
    if (app::state().anomaly.kind == app::AnomalyKind::None) return;
    render_from_state();
    lv_obj_clear_flag(g_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_backdrop);
    app::state().anomaly_visible = true;
}

void anomaly_modal_dismiss() {
    lv_obj_add_flag(g_backdrop, LV_OBJ_FLAG_HIDDEN);
    app::state().anomaly_visible = false;
}

void anomaly_modal_resolve() {
    app::mock_resolve_anomaly();
    lv_obj_add_flag(g_backdrop, LV_OBJ_FLAG_HIDDEN);
    status_bar_set_anomaly_count(0);
}

void anomaly_modal_raise(app::AnomalyKind kind) {
    app::mock_raise_anomaly(kind);
    status_bar_set_anomaly_count(1);
    anomaly_modal_open_current();
}

} // namespace ui
