// CONFIG-SLOTS -- rack identity, RS485 chains, slot numbering.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"

#include <stdio.h>

namespace ui::screens {

void build_config_slots(lv_obj_t* body) {
    lv_obj_t* sc = form_scroller(body);

    // Rack identity
    {
        lv_obj_t* fc = form_card(sc, "RACK IDENTITY");

        lv_obj_t* r = form_row(fc);
        form_row_label(r, "Rack name", nullptr);
        form_input(r, "Lab Rack A", 200);

        r = form_row(fc);
        form_row_label(r, "Inventree location", "Sub-locations are slot numbers");
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", app::state().inv_location_id);
        form_input(r, buf, 0, true);
    }

    // RS485 chains
    {
        lv_obj_t* fc = form_card(sc, "RS485 CHAINS");
        for (int chain = 1; chain <= app::N_CHAINS; ++chain) {
            int n_on_chain = app::SLOTS_PER_CHAIN;
            int start_num = (chain - 1) * app::SLOTS_PER_CHAIN + 1;

            lv_obj_t* r = form_row(fc);
            char title[16]; snprintf(title, sizeof(title), "Chain %d", chain);
            char desc[40];
            snprintf(desc, sizeof(desc), "Auto-detected  %d slots downstream", n_on_chain);
            form_row_label(r, title, desc);

            char sn[8]; snprintf(sn, sizeof(sn), "%d", start_num);
            form_input(r, sn, 0, true);
            char cn[8]; snprintf(cn, sizeof(cn), "%d", n_on_chain);
            form_input(r, cn, 0, true);
            button(r, "Re-scan", BtnKind::Default);
        }
    }

    // Numbering
    {
        lv_obj_t* fc = form_card(sc, "NUMBERING");

        lv_obj_t* r = form_row(fc);
        form_row_label(r, "Direction", "Match the labels on the rack");
        form_input(r, "Left-to-right, top-to-bottom", 240);

        r = form_row(fc);
        form_row_label(r, "Skip numbers", "Comma-separated slot numbers to omit");
        form_input(r, "(none)", 200);

        r = form_row(fc);
        // empty left col
        lv_obj_t* spacer = lv_obj_create(r);
        lv_obj_remove_style_all(spacer);
        lv_obj_set_flex_grow(spacer, 1);
        lv_obj_set_height(spacer, 1);
        lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
        button(r, "Save numbering", BtnKind::Primary);
    }
}

} // namespace ui::screens
