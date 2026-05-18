// VIEW -- full inventory list of occupied slots.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/app_state.h"

#include <stdio.h>

namespace ui::screens {

struct PickCtx { int slot_num; };

static void on_manual_pick(lv_event_t* e) {
    auto* c = static_cast<PickCtx*>(lv_event_get_user_data(e));
    if (!c) return;
    app::mock_manual_pick(c->slot_num);
    ui::rebuild_current();
}
static void free_pick_ctx(lv_event_t* e) {
    delete static_cast<PickCtx*>(lv_event_get_user_data(e));
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

        auto* ctx = new PickCtx{ s.slot };
        lv_obj_t* btn = row_add_button(row, "Pick", on_manual_pick, ctx);
        lv_obj_add_event_cb(btn, free_pick_ctx, LV_EVENT_DELETE, ctx);
    }

    if (n_shown == 0) {
        empty_state(sc, "Rack is empty.");
    }
}

} // namespace ui::screens
