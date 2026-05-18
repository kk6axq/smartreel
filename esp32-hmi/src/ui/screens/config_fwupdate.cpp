// CONFIG-FWUPDATE -- firmware version + actions.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"

namespace ui::screens {

using namespace theme;

void build_config_fwupdate(lv_obj_t* body) {
    lv_obj_t* sc = form_scroller(body);

    {
        lv_obj_t* fc = form_card(sc, "FIRMWARE");

        lv_obj_t* r = form_row(fc);
        form_row_label(r, "Current version", "Built 2026-04-21");
        lv_obj_t* l = lv_label_create(r);
        lv_label_set_text(l, app::state().fw_version);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, color::text(), 0);

        r = form_row(fc);
        form_row_label(r, "Latest available", "Inventree OTA channel");
        l = lv_label_create(r);
        lv_label_set_text(l, "v0.4.2 (current)");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, color::slot_picked(), 0);

        r = form_row(fc);
        form_row_label(r, "Channel", nullptr);
        form_input(r, "stable", 140);

        r = form_row(fc);
        form_row_label(r, "Auto-check on boot", nullptr);
        form_toggle(r, true);
    }

    {
        lv_obj_t* fc = form_card(sc, "ACTIONS");
        lv_obj_t* r = lv_obj_create(fc);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(r, 8, 0);
        lv_obj_set_style_pad_ver(r, 4, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);

        button(r, "Check now",       BtnKind::Primary);
        button(r, "Update from USB", BtnKind::Default);
        lv_obj_t* disabled = button(r, "Install update", BtnKind::Default);
        lv_obj_add_style(disabled, const_cast<lv_style_t*>(&theme::s().btn_disabled), 0);
        lv_obj_add_state(disabled, LV_STATE_DISABLED);
    }
}

} // namespace ui::screens
