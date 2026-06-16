#include "ui/notify.h"

#include "ui/theme.h"
#include "ui/widgets.h"

#include <lvgl.h>
#include <stdio.h>

namespace ui {

using namespace theme;

namespace {

lv_color_t mood_color(NotifyMood m) {
    switch (m) {
        case NotifyMood::Success: return color::slot_picked();
        case NotifyMood::Warn:    return color::slot_warn();
        case NotifyMood::Error:   return color::slot_error();
        case NotifyMood::Info:
        default:                  return color::accent();
    }
}

// Replace a button()'s centred label text (button() makes the label child 0).
void set_btn_text(lv_obj_t* btn, const char* text) {
    if (!btn) return;
    lv_obj_t* l = lv_obj_get_child(btn, 0);
    if (l) lv_label_set_text(l, text);
}

// ---- Toast ----------------------------------------------------------------
lv_obj_t*    g_toast_box = nullptr;
lv_obj_t*    g_toast_lbl = nullptr;
lv_timer_t*  g_toast_timer = nullptr;

void toast_hide_cb(lv_timer_t* t) {
    if (g_toast_box) lv_obj_add_flag(g_toast_box, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
    if (t == g_toast_timer) g_toast_timer = nullptr;
}

// ---- Confirm --------------------------------------------------------------
lv_obj_t* g_cf_backdrop = nullptr;
lv_obj_t* g_cf_header   = nullptr;
lv_obj_t* g_cf_title    = nullptr;
lv_obj_t* g_cf_msg      = nullptr;
lv_obj_t* g_cf_ok_btn   = nullptr;
lv_obj_t* g_cf_cancel   = nullptr;
void   (*g_cf_cb)(void*) = nullptr;
void*    g_cf_user       = nullptr;

void confirm_hide() {
    if (g_cf_backdrop) lv_obj_add_flag(g_cf_backdrop, LV_OBJ_FLAG_HIDDEN);
}
void cf_on_ok(lv_event_t*) {
    auto cb = g_cf_cb; void* u = g_cf_user;
    g_cf_cb = nullptr; g_cf_user = nullptr;
    confirm_hide();
    if (cb) cb(u);          // run after hide: cb may open another overlay
}
void cf_on_cancel(lv_event_t*) {
    g_cf_cb = nullptr; g_cf_user = nullptr;
    confirm_hide();
}
void cf_on_backdrop(lv_event_t*) { /* tap outside: keep open */ }

// ---- Pick prompt ----------------------------------------------------------
lv_obj_t* g_pp_backdrop = nullptr;
lv_obj_t* g_pp_header   = nullptr;
lv_obj_t* g_pp_title    = nullptr;
lv_obj_t* g_pp_msg      = nullptr;
lv_obj_t* g_pp_cancel   = nullptr;
int       g_pp_slot     = -1;
bool      g_pp_done     = false;
void   (*g_pp_cancel_cb)(void*) = nullptr;
void*    g_pp_user      = nullptr;
lv_timer_t* g_pp_close_timer = nullptr;

void pp_hide() {
    if (g_pp_backdrop) lv_obj_add_flag(g_pp_backdrop, LV_OBJ_FLAG_HIDDEN);
    g_pp_slot = -1;
    g_pp_done = false;
}
void pp_close_cb(lv_timer_t* t) {
    pp_hide();
    lv_timer_del(t);
    if (t == g_pp_close_timer) g_pp_close_timer = nullptr;
}
void pp_on_cancel(lv_event_t*) {
    if (!g_pp_done && g_pp_cancel_cb) g_pp_cancel_cb(g_pp_user);
    pp_hide();
}
void pp_on_backdrop(lv_event_t*) { /* tap outside: keep open */ }

// Shared: a centred modal box with a coloured header bar, a message, and
// an actions row. Returns the box; out-params hand back the pieces callers
// restyle/relabel per open.
lv_obj_t* make_modal(lv_obj_t* backdrop, lv_obj_t** header_out,
                     lv_obj_t** title_out, lv_obj_t** msg_out) {
    lv_obj_t* box = lv_obj_create(backdrop);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, const_cast<lv_style_t*>(&theme::s().modal_box), 0);
    lv_obj_set_size(box, 480, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);    // eat clicks

    lv_obj_t* header = lv_obj_create(box);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, const_cast<lv_style_t*>(&theme::s().modal_header_info), 0);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* title = lv_label_create(header);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t* body = lv_obj_create(box);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(body, 16, 0);
    lv_obj_set_style_pad_ver(body, 14, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* msg = lv_label_create(body);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(msg, color::text(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));

    *header_out = header; *title_out = title; *msg_out = msg;
    return box;
}

lv_obj_t* make_backdrop(lv_event_cb_t on_tap) {
    lv_obj_t* bd = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, layout::SCREEN_W, layout::SCREEN_H);
    lv_obj_set_pos(bd, 0, 0);
    lv_obj_set_style_bg_color(bd, color::backdrop(), 0);
    lv_obj_set_style_bg_opa(bd, color::backdrop_opa, 0);
    lv_obj_set_flex_flow(bd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bd, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bd, on_tap, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_HIDDEN);
    return bd;
}

// Set a header bar's mood colour (the modal box base is neutral).
void set_header_mood(lv_obj_t* header, NotifyMood m) {
    lv_obj_set_style_bg_color(header, mood_color(m), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
}

} // namespace

void notify_init() {
    // ---- Toast: floating box on lv_layer_top, no backdrop ----
    g_toast_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_toast_box);
    lv_obj_set_size(g_toast_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(g_toast_box, 560, 0);
    lv_obj_set_style_radius(g_toast_box, layout::RADIUS, 0);
    lv_obj_set_style_bg_color(g_toast_box, color::surface(), 0);
    lv_obj_set_style_bg_opa(g_toast_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_toast_box, 2, 0);
    lv_obj_set_style_pad_hor(g_toast_box, 18, 0);
    lv_obj_set_style_pad_ver(g_toast_box, 12, 0);
    lv_obj_clear_flag(g_toast_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_toast_box, LV_OBJ_FLAG_CLICKABLE);   // don't steal touch
    lv_obj_align(g_toast_box, LV_ALIGN_TOP_MID, 0, layout::STATUS_H + 8);
    lv_obj_add_flag(g_toast_box, LV_OBJ_FLAG_HIDDEN);
    g_toast_lbl = lv_label_create(g_toast_box);
    lv_obj_set_style_text_font(g_toast_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_toast_lbl, color::text(), 0);
    // Let the label size to its text. Sizing it LV_PCT(100) inside a
    // content-sized box collapses it to ~0 width, so the box renders as an
    // empty rounded rectangle (review items 1/4). The box max_width caps long
    // strings; toasts are short one-liners.
    lv_obj_set_width(g_toast_lbl, LV_SIZE_CONTENT);

    // ---- Confirm ----
    g_cf_backdrop = make_backdrop(cf_on_backdrop);
    make_modal(g_cf_backdrop, &g_cf_header, &g_cf_title, &g_cf_msg);
    lv_obj_t* box = lv_obj_get_child(g_cf_backdrop, 0);
    lv_obj_t* actions = lv_obj_create(box);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(actions, 16, 0);
    lv_obj_set_style_pad_bottom(actions, 14, 0);
    lv_obj_set_style_pad_gap(actions, 8, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    g_cf_cancel = button(actions, "Cancel", BtnKind::Default, cf_on_cancel);
    lv_obj_set_flex_grow(g_cf_cancel, 1);
    g_cf_ok_btn = button(actions, "Confirm", BtnKind::Primary, cf_on_ok);
    lv_obj_set_flex_grow(g_cf_ok_btn, 1);

    // ---- Pick prompt ----
    g_pp_backdrop = make_backdrop(pp_on_backdrop);
    make_modal(g_pp_backdrop, &g_pp_header, &g_pp_title, &g_pp_msg);
    lv_obj_t* ppbox = lv_obj_get_child(g_pp_backdrop, 0);
    lv_obj_t* ppact = lv_obj_create(ppbox);
    lv_obj_remove_style_all(ppact);
    lv_obj_set_size(ppact, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(ppact, 16, 0);
    lv_obj_set_style_pad_bottom(ppact, 14, 0);
    lv_obj_set_flex_flow(ppact, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(ppact, LV_OBJ_FLAG_SCROLLABLE);
    g_pp_cancel = button(ppact, "Cancel", BtnKind::Default, pp_on_cancel);
    lv_obj_set_flex_grow(g_pp_cancel, 1);
}

void toast(const char* message, NotifyMood mood, int ms) {
    if (!g_toast_box) return;
    if (ms <= 0) ms = 2000;
    lv_label_set_text(g_toast_lbl, message ? message : "");
    lv_obj_set_style_border_color(g_toast_box, mood_color(mood), 0);
    lv_obj_clear_flag(g_toast_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_toast_box);
    if (g_toast_timer) { lv_timer_del(g_toast_timer); g_toast_timer = nullptr; }
    g_toast_timer = lv_timer_create(toast_hide_cb, ms, nullptr);
    lv_timer_set_repeat_count(g_toast_timer, 1);
}

void confirm(const ConfirmOpts& opts) {
    if (!g_cf_backdrop) return;
    lv_label_set_text(g_cf_title, opts.title ? opts.title : "Confirm");
    lv_label_set_text(g_cf_msg,   opts.message ? opts.message : "");
    set_header_mood(g_cf_header, opts.danger ? NotifyMood::Error : NotifyMood::Info);
    set_btn_text(g_cf_ok_btn, opts.confirm_label ? opts.confirm_label : "Confirm");
    set_btn_text(g_cf_cancel, opts.cancel_label ? opts.cancel_label : "Cancel");
    // Danger restyle: swap the primary overlay for the danger one.
    lv_obj_remove_style(g_cf_ok_btn, const_cast<lv_style_t*>(&theme::s().btn_primary), 0);
    lv_obj_remove_style(g_cf_ok_btn, const_cast<lv_style_t*>(&theme::s().btn_danger), 0);
    lv_obj_add_style(g_cf_ok_btn, opts.danger
        ? const_cast<lv_style_t*>(&theme::s().btn_danger)
        : const_cast<lv_style_t*>(&theme::s().btn_primary), 0);
    g_cf_cb   = opts.on_confirm;
    g_cf_user = opts.user;
    lv_obj_clear_flag(g_cf_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_cf_backdrop);
}

void pick_prompt_open(int slot_num, void (*on_cancel)(void*), void* user) {
    if (!g_pp_backdrop) return;
    if (g_pp_close_timer) { lv_timer_del(g_pp_close_timer); g_pp_close_timer = nullptr; }
    g_pp_slot      = slot_num;
    g_pp_done      = false;
    g_pp_cancel_cb = on_cancel;
    g_pp_user      = user;
    char title[32]; snprintf(title, sizeof(title), "Pick slot #%d", slot_num);
    char msg[120];
    snprintf(msg, sizeof(msg),
             "Remove the reel from slot #%d. It transfers to staging once "
             "you pull it.", slot_num);
    lv_label_set_text(g_pp_title, title);
    lv_label_set_text(g_pp_msg, msg);
    set_header_mood(g_pp_header, NotifyMood::Info);
    set_btn_text(g_pp_cancel, "Cancel");
    lv_obj_clear_flag(g_pp_cancel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_pp_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_pp_backdrop);
}

void pick_prompt_done(int slot_num) {
    if (!g_pp_backdrop || g_pp_slot != slot_num || g_pp_done) return;
    g_pp_done = true;
    char msg[96];
    snprintf(msg, sizeof(msg), "Reel removed from slot #%d -- transferred to staging.",
             slot_num);
    lv_label_set_text(g_pp_title, "Picked");
    lv_label_set_text(g_pp_msg, msg);
    set_header_mood(g_pp_header, NotifyMood::Success);
    lv_obj_add_flag(g_pp_cancel, LV_OBJ_FLAG_HIDDEN);   // nothing to cancel now
    g_pp_close_timer = lv_timer_create(pp_close_cb, 1500, nullptr);
    lv_timer_set_repeat_count(g_pp_close_timer, 1);
}

bool pick_prompt_active(int slot_num) {
    return g_pp_backdrop
        && !lv_obj_has_flag(g_pp_backdrop, LV_OBJ_FLAG_HIDDEN)
        && g_pp_slot == slot_num
        && !g_pp_done;
}

} // namespace ui
