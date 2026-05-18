// CONFIG-DIVIDERS -- maintenance toggle + empty-slot list + recent changes.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"

#include <stdio.h>

namespace ui::screens {

using namespace theme;

void build_config_dividers(lv_obj_t* body) {
    lv_obj_t* sc = form_scroller(body);

    // Maintenance mode toggle
    {
        lv_obj_t* fc = form_card(sc, "DIVIDER MAINTENANCE MODE");
        lv_obj_t* r = form_row(fc);
        form_row_label(r, "Enable changes",
            "While ON, divider button changes are accepted instead of raising "
            "anomalies. Slot must be empty.");
        form_toggle(r, false);
    }

    // Empty slot list (first 6)
    {
        lv_obj_t* fc = form_card(sc, "EMPTY SLOTS (SAFE TO MODIFY)");
        int shown = 0;
        for (auto& s : app::state().rack) {
            if (s.state != app::SlotState::EMPTY) continue;
            if (shown >= 6) break;
            shown++;
            lv_obj_t* r = form_row(fc);
            char title[16]; snprintf(title, sizeof(title), "Slot %d", s.slot);
            char desc[40];
            snprintf(desc, sizeof(desc), "Chain %d  position %d", s.chain, s.position);
            form_row_label(r, title, desc);
            lv_obj_t* l = lv_label_create(r);
            lv_label_set_text(l, "empty  LED off");
            lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(l, color::text_muted(), 0);
        }
        if (shown == 0) {
            lv_obj_t* l = lv_label_create(fc);
            lv_label_set_text(l, "All slots are occupied.");
            lv_obj_set_style_text_color(l, color::text_muted(), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
            lv_obj_set_style_pad_all(l, 8, 0);
        }
    }

    // Recent changes (mock)
    {
        lv_obj_t* fc = form_card(sc, "RECENT DIVIDER CHANGES");

        lv_obj_t* r = form_row(fc);
        form_row_label(r, "Slot 12 / 13", "Divider added  14:02");
        lv_obj_t* l = lv_label_create(r);
        lv_label_set_text(l, "applied");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, color::text_muted(), 0);

        r = form_row(fc);
        form_row_label(r, "Slot 28 / 29", "Divider removed  11:45");
        l = lv_label_create(r);
        lv_label_set_text(l, "applied");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, color::text_muted(), 0);
    }
}

} // namespace ui::screens
