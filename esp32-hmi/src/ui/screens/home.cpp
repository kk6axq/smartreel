// HOME -- dot grid + 4 action tiles (Load / View / Pick / Configure).
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"

namespace ui::screens {

using namespace theme;

static void on_load(lv_event_t*)      { ui::navigate(Screen::Load); }
static void on_view(lv_event_t*)      { ui::navigate(Screen::View); }
static void on_pick(lv_event_t*)      { ui::navigate(Screen::PickList); }
static void on_configure(lv_event_t*) { ui::navigate(Screen::Configure); }

void build_home(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    DotGridOpts dg = {};
    dg.show_legend  = true;
    dg.header_label = "Rack overview";
    dg.clickable    = false;
    dot_grid_card(body, dg);

    lv_obj_t* tiles_box = lv_obj_create(body);
    lv_obj_remove_style_all(tiles_box);
    lv_obj_set_size(tiles_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(tiles_box, 1);
    lv_obj_set_layout(tiles_box, LV_LAYOUT_GRID);
    static lv_coord_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t rows[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(tiles_box, cols, rows);
    lv_obj_set_style_pad_column(tiles_box, 10, 0);
    lv_obj_set_style_pad_row(tiles_box, 10, 0);
    lv_obj_clear_flag(tiles_box, LV_OBJ_FLAG_SCROLLABLE);

    auto place = [tiles_box](lv_obj_t* o, int col, int row) {
        lv_obj_set_grid_cell(o, LV_GRID_ALIGN_STRETCH, col, 1,
                                 LV_GRID_ALIGN_STRETCH, row, 1);
    };
    auto mktile = [&](const char* l, const char* s, lv_event_cb_t cb) {
        TileOpts to;
        to.label = l; to.sublabel = s; to.on_click = cb; to.user = nullptr;
        return tile(tiles_box, to);
    };
    place(mktile("Load",      "Scan a part to add it", on_load),      0, 0);
    place(mktile("View",      "Browse rack contents",  on_view),      1, 0);
    place(mktile("Pick",      "Run a pick job",        on_pick),      0, 1);
    place(mktile("Configure", "Setup & maintenance",   on_configure), 1, 1);
}

} // namespace ui::screens
