// VIEW -- full inventory list of occupied slots.
//
// Each row has:
//   * "Find"  -- lights the slot's LED in theme blue for 3 s so you can
//                spot the physical slot.
//   * "Pick"  -- arms a pick-out: lights the slot and waits for you to
//                physically pull that reel. The reel-presence switch
//                releasing is the confirmation -- main.cpp's
//                apply_slot_change() then removes it from inventory and
//                drops the light. While armed the button reads "Cancel",
//                which turns the light off and aborts without removing.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/app_state.h"
#include "ui/notify.h"
#include "app/leds.h"
#include "app/inv_sync.h"

#include <stdint.h>
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

// ---- Pick out (take a reel out of inventory) ----------------------
// Arming is a UI action; the *confirmation* is physical. Pressing Pick
// lights the slot and records it as the pending pick-out. When the reel
// is actually pulled, the hardware presence change reaches
// apply_slot_change(), which removes it from inventory. Cancel just
// drops the light and clears the pending marker.
struct SlotCtx { int slot_num; };

// Cancel from the pick-prompt modal (review item 16): drop the light and
// disarm. Runs when the operator dismisses the "remove the reel" popup
// before pulling the reel. The hardware-removal path (main.cpp) flips the
// same modal to a success confirmation instead of calling this.
static void on_prompt_cancel(void* user) {
    const int slot = (int)(intptr_t)user;
    leds::light_slot(slot, 0, 0, 0);
    app::cancel_pick_out();
    if (ui::current() == ui::Screen::View) ui::rebuild_current();
}

static void on_pick(lv_event_t* e) {
    auto* c = static_cast<SlotCtx*>(lv_event_get_user_data(e));
    if (!c) return;
    if (!inv_sync::online()) return;   // picking is online-only (user stories)
    // Re-arming onto a different slot: drop the previously lit one.
    const int prev = app::state().pick_out_slot;
    if (prev > 0 && prev != c->slot_num) leds::light_slot(prev, 0, 0, 0);
    app::begin_pick_out(c->slot_num);
    leds::light_slot(c->slot_num, 0x25, 0x63, 0xEB);   // theme blue
    // Pop the "remove the reel" prompt (review item 16). The modal backdrop
    // covers the inline Pick/Cancel toggle; pulling the reel confirms and
    // closes it via main.cpp's finish_pick_out path.
    ui::pick_prompt_open(c->slot_num, on_prompt_cancel,
                         (void*)(intptr_t)c->slot_num);
    ui::rebuild_current();
}
static void on_pick_cancel(lv_event_t* e) {
    auto* c = static_cast<SlotCtx*>(lv_event_get_user_data(e));
    if (!c) return;
    leds::light_slot(c->slot_num, 0, 0, 0);
    app::cancel_pick_out();
    ui::rebuild_current();
}
static void free_slot_ctx(lv_event_t* e) {
    delete static_cast<SlotCtx*>(lv_event_get_user_data(e));
}

// Rack overview is reached from here now (review item 1: Rack is a
// sub-page of View, not a peer tile on Home).
static void on_rack_view(lv_event_t*) { ui::navigate(ui::Screen::RackGrid); }

void build_view(lv_obj_t* body) {
    lv_obj_t* sc = row_scroller(body);

    // Rack overview lives under View now (review item 1). A right-aligned
    // header button drills into the spatial dot-grid; back returns here.
    {
        lv_obj_t* hdr = lv_obj_create(sc);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_END,
                                   LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
        button(hdr, "Rack overview", BtnKind::Default, on_rack_view);
    }

    const bool online = inv_sync::online();
    if (!online) {
        banner(sc, "InvenTree is unreachable - Find works, but picking is "
                   "disabled until the connection comes back.",
               /*warn=*/true);
    }

    int n_shown = 0;
    app::State& st = app::state();
    for (int i = 0; i < st.n_rack; ++i) {
        const app::Slot& s = st.rack[i];
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

        // Pick toggles to Cancel while this slot is the armed pick-out.
        // Disabled offline: a pick the server never hears about would
        // desync the rack (cancel stays enabled to back out).
        const bool armed = (st.pick_out_slot == s.slot);
        auto* pctx = new SlotCtx{ s.slot };
        lv_obj_t* pbtn = row_add_button(row, armed ? "Cancel" : "Pick",
                                        armed ? on_pick_cancel : on_pick, pctx,
                                        /*muted=*/!armed,
                                        /*success=*/false,
                                        /*disabled=*/!armed && !online);
        lv_obj_add_event_cb(pbtn, free_slot_ctx, LV_EVENT_DELETE, pctx);
    }

    if (n_shown == 0) {
        empty_state(sc, "Rack is empty.");
    }
}

} // namespace ui::screens
