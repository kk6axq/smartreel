// CONFIG-DIVIDERS -- commission the slot/divider layout and review the
// resulting logical slots.
//
// Pre-commission the rack follows live hardware: physically pull a
// divider between two slots and they merge into one wider logical slot
// (the lower number wins; the upper number disappears). "Commission"
// snapshots the current live module counts + divider layout as the
// expected config; after that, a divider that should be present going
// missing raises an alarm instead of re-shaping the rack.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "ui/screen_manager.h"
#include "app/hw_mirror.h"
#include "app/slot_map.h"
#include "storage/config_store.h"

#include <stdio.h>

namespace ui::screens {

using namespace theme;

// Capture the live topology + divider layout as the committed config.
static void on_commission(lv_event_t*) {
    auto& rc = config_store::cfg().rack;
    for (int p = 0; p < app::N_PORTS; ++p)
        rc.module_count[p] = hw_mirror::module_count(p);
    rc.dividers.clear();
    for (int p = 0; p < app::N_PORTS; ++p)
        for (int m = 0; m < rc.module_count[p]; ++m)
            for (int s = 0; s < 16; ++s) {
                if (m == rc.module_count[p] - 1 && s == 15) continue;   // port edge (right of last slot)
                if (!hw_mirror::divider_present(p, m, s))
                    rc.dividers.set_pulled((uint8_t)p, (uint8_t)m, (uint8_t)s, true);
            }
    rc.committed = true;
    config_store::save_rack();
    app::rebuild_rack();
    ui::mark_all_dirty();
}

// Drop back to following live hardware (edit mode).
static void on_uncommit(lv_event_t*) {
    config_store::cfg().rack.committed = false;
    config_store::save_rack();
    app::rebuild_rack();
    ui::mark_all_dirty();
}

void build_config_dividers(lv_obj_t* body) {
    lv_obj_t* sc = form_scroller(body);
    app::State& st = app::state();
    const auto& rc = config_store::cfg().rack;

    // Commissioning status + actions
    {
        lv_obj_t* fc = form_card(sc, "COMMISSIONING");

        lv_obj_t* r = form_row(fc);
        form_row_label(r, "Status",
            rc.committed ? "Committed - divider changes raise an alarm"
                         : "Edit mode - following live hardware");
        form_input(r, rc.committed ? "committed" : "live", 0, true);

        r = form_row(fc);
        form_row_label(r, "Combine slots",
            "In edit mode, physically pull a divider between two slots to merge "
            "them into one wider slot, then Commission to lock the layout.");

        r = form_row(fc);
        lv_obj_t* spacer = lv_obj_create(r);
        lv_obj_remove_style_all(spacer);
        lv_obj_set_flex_grow(spacer, 1);
        lv_obj_set_height(spacer, 1);
        lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
        button(r, rc.committed ? "Re-commission" : "Commission layout",
               BtnKind::Primary, on_commission);
        if (rc.committed) button(r, "Edit", BtnKind::Default, on_uncommit);
    }

    // Resulting logical slots, per port
    {
        lv_obj_t* fc = form_card(sc, "LOGICAL SLOTS");
        if (st.n_rack == 0) {
            lv_obj_t* l = lv_label_create(fc);
            lv_label_set_text(l, "No reel modules detected.");
            lv_obj_set_style_text_color(l, color::text_muted(), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
            lv_obj_set_style_pad_all(l, 8, 0);
            return;
        }
        for (int i = 0; i < st.n_rack; ++i) {
            const app::Slot& s = st.rack[i];
            lv_obj_t* r = form_row(fc);
            char title[16]; snprintf(title, sizeof(title), "Slot %d", s.slot);
            char desc[48];
            if (s.width > 1)
                snprintf(desc, sizeof(desc), "Port %d  -  %d slots combined", s.port + 1, s.width);
            else
                snprintf(desc, sizeof(desc), "Port %d  -  standard width", s.port + 1);
            form_row_label(r, title, desc);

            char val[16];
            if (s.width > 1) snprintf(val, sizeof(val), "x%d", s.width);
            else             snprintf(val, sizeof(val), "1");
            form_input(r, val, 0, true);
        }
    }
}

} // namespace ui::screens
