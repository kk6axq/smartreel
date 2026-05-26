// VIEW -- full inventory list of occupied slots.
//
// Each row has a "Find" button that lights the slot's LED in theme
// blue for 3 seconds via an lv_timer one-shot. Useful when you
// know what you're looking for and need the rack to tell you which
// physical slot it lives in.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/app_state.h"
#include "app/leds.h"

#include <stdio.h>

namespace ui::screens {

struct FindCtx { int slot_num; };

// lv_timer one-shot to clear the LED 3 seconds after Find press.
// LVGL 8.4 exposes user_data as a struct member (lv_timer_get_user_data
// is v9 only).
static void clear_one_cb(lv_timer_t* t) {
    auto* c = static_cast<FindCtx*>(t->user_data);
    if (c) {
        leds::light_slot(c->slot_num, 0, 0, 0);
        delete c;
    }
    lv_timer_del(t);
}

static void on_find(lv_event_t* e) {
    auto* c = static_cast<FindCtx*>(lv_event_get_user_data(e));
    if (!c) return;
    // Light blue for 3 s. Each press allocates a fresh clear-ctx
    // because two Finds could overlap.
    leds::light_slot(c->slot_num, 0x25, 0x63, 0xEB);
    auto* clear = new FindCtx{ c->slot_num };
    lv_timer_t* t = lv_timer_create(clear_one_cb, 3000, clear);
    lv_timer_set_repeat_count(t, 1);
}
static void free_find_ctx(lv_event_t* e) {
    delete static_cast<FindCtx*>(lv_event_get_user_data(e));
}

void build_view(lv_obj_t* body) {
    lv_obj_t* sc = row_scroller(body);

    int n_shown = 0;
    for (auto& s : app::state().rack) {
        if (s.state != app::SlotState::OCCUPIED) continue;
        if (!s.part.valid) continue;
        n_shown++;

        RowOpts ro; ro.stripe_state = s.state;
        lv_obj_t* row = row_make(sc, ro);

        char tag[16]; snprintf(tag, sizeof(tag), "#%d", s.slot);
        row_add_slot_num(row, tag);

        char meta[64];
        snprintf(meta, sizeof(meta), "%s  %s", s.part.id, s.part.pkg);
        row_add_main_two_line(row, s.part.name, meta);

        char qb[16]; snprintf(qb, sizeof(qb), "%d", s.qty);
        row_add_qty_two_line(row, qb, "on reel");

        auto* ctx = new FindCtx{ s.slot };
        lv_obj_t* btn = row_add_button(row, "Find", on_find, ctx);
        lv_obj_add_event_cb(btn, free_find_ctx, LV_EVENT_DELETE, ctx);
    }

    if (n_shown == 0) {
        empty_state(sc, "Rack is empty.");
    }
}

} // namespace ui::screens
