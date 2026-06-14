#include "ui/locate_overlay.h"

#include "ui/theme.h"
#include "ui/widgets.h"
#include "app/leds.h"
#include "ui/app_state.h"   // app::MAX_LOGICAL_SLOTS

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace ui {

using namespace theme;

namespace {

// Highlight colour: theme TEAL, distinct from the View "Find" theme-blue
// and the amber "moved in InvenTree, remove me" so an operator can tell a
// web-UI locate apart from a local find or an anomaly at a glance.
constexpr uint8_t LOC_R = 0x14, LOC_G = 0xB8, LOC_B = 0xA6;

// Active locate set -- LVGL-task only, so no lock. Sized to the rack.
bool s_active[app::MAX_LOGICAL_SLOTS + 1] = {};
int  s_n_active = 0;

lv_obj_t* g_backdrop = nullptr;
lv_obj_t* g_box      = nullptr;
lv_obj_t* g_msg      = nullptr;

void clear_all_locates() {
    for (int n = 1; n <= app::MAX_LOGICAL_SLOTS; ++n) {
        if (s_active[n]) {
            leds::light_slot(n, 0, 0, 0);   // restore resting (dark) state
            s_active[n] = false;
        }
    }
    s_n_active = 0;
}

void hide() {
    if (g_backdrop) lv_obj_add_flag(g_backdrop, LV_OBJ_FLAG_HIDDEN);
}

void on_dismiss(lv_event_t*) {
    // Dismiss clears EVERY active locate at once (simplest UX; the
    // overlay shows the count/list so the operator knows what they're
    // clearing). Turns the LEDs back off and hides the overlay.
    clear_all_locates();
    hide();
}

void on_backdrop(lv_event_t*) { /* tap outside: keep open */ }

void refresh_message() {
    char buf[160];
    if (s_n_active <= 0) { buf[0] = 0; }
    else {
        // List up to the first few slots; summarise the rest.
        int written = snprintf(buf, sizeof(buf),
            s_n_active == 1 ? "InvenTree asked to locate slot "
                            : "InvenTree asked to locate slots ");
        if (written < 0 || written >= (int)sizeof(buf)) written = (int)sizeof(buf) - 1;
        bool first = true;
        int shown = 0;
        for (int n = 1; n <= app::MAX_LOGICAL_SLOTS && written < (int)sizeof(buf) - 1; ++n) {
            if (!s_active[n]) continue;
            if (shown >= 8) {
                written += snprintf(buf + written, sizeof(buf) - written, ", ...");
                break;
            }
            int w = snprintf(buf + written, sizeof(buf) - written,
                             "%s%d", first ? "" : ", ", n);
            if (w > 0) written += w;
            if (written >= (int)sizeof(buf)) { written = (int)sizeof(buf) - 1; break; }
            first = false;
            shown++;
        }
        if (written < (int)sizeof(buf) - 1)
            snprintf(buf + written, sizeof(buf) - written,
                     ". Lit on the rack -- press Dismiss when done.");
    }
    if (g_msg) lv_label_set_text(g_msg, buf);
}

} // anonymous namespace

void locate_overlay_init() {
    // Backdrop: like the anomaly modal, but transparent so the operator
    // can still read the screen behind it. Hidden by default.
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

    // Modal box (info mood).
    g_box = lv_obj_create(g_backdrop);
    lv_obj_remove_style_all(g_box);
    lv_obj_add_style(g_box, const_cast<lv_style_t*>(&theme::s().modal_box), 0);
    lv_obj_add_style(g_box, const_cast<lv_style_t*>(&theme::s().modal_box_info), 0);
    lv_obj_set_size(g_box, 480, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(g_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_box, LV_OBJ_FLAG_CLICKABLE);   // eat clicks

    // Header (info styled).
    lv_obj_t* header = lv_obj_create(g_box);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, const_cast<lv_style_t*>(&theme::s().modal_header_info), 0);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Locate");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    // Body message.
    lv_obj_t* body = lv_obj_create(g_box);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(body, 16, 0);
    lv_obj_set_style_pad_ver(body, 14, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    g_msg = lv_label_create(body);
    lv_label_set_text(g_msg, "");
    lv_obj_set_style_text_font(g_msg, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_msg, color::text(), 0);
    lv_label_set_long_mode(g_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_msg, LV_PCT(100));

    // Action: single Dismiss button (clears all).
    lv_obj_t* actions = lv_obj_create(g_box);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(actions, 16, 0);
    lv_obj_set_style_pad_bottom(actions, 14, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* dismiss = button(actions, "Dismiss", BtnKind::Primary, on_dismiss);
    lv_obj_set_flex_grow(dismiss, 1);
}

void locate_overlay_add(int slot_num) {
    if (slot_num < 1 || slot_num > app::MAX_LOGICAL_SLOTS) return;
    if (!s_active[slot_num]) {
        s_active[slot_num] = true;
        s_n_active++;
        leds::light_slot(slot_num, LOC_R, LOC_G, LOC_B);   // persistent
    }
    refresh_message();
    if (g_backdrop) {
        lv_obj_clear_flag(g_backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_backdrop);
    }
}

bool locate_overlay_active() { return s_n_active > 0; }

} // namespace ui
