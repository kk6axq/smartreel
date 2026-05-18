#include "ui/screens/screens.h"
#include "ui/theme.h"

namespace ui::screens {

using namespace theme;

lv_obj_t* form_scroller(lv_obj_t* body) {
    lv_obj_t* sc = lv_obj_create(body);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(sc, 12, 0);
    lv_obj_set_style_pad_ver(sc, 10, 0);
    lv_obj_set_style_pad_gap(sc, 8, 0);     // gap between form_cards
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
    return sc;
}

lv_obj_t* row_scroller(lv_obj_t* body) {
    lv_obj_t* sc = lv_obj_create(body);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(sc, 10, 0);
    lv_obj_set_style_pad_ver(sc, 8, 0);
    lv_obj_set_style_pad_gap(sc, 6, 0);     // gap between rows
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
    return sc;
}

} // namespace ui::screens
