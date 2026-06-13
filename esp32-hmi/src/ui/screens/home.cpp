// HOME -- big action tiles (Load / View / Pick / Rack / Settings).
// The slot-occupancy dot grid lives on its own screen now (RackGrid);
// the 4.3" panel is too small to share it with the tiles at readable
// font sizes (user-stories.md, UI Updates).
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "app/leds.h"

#include <stdio.h>

namespace ui::screens {

using namespace theme;

static void on_load(lv_event_t*) {
    // Always enter Load on a fresh scan: drop any half-finished load
    // (a locked/placed part or lit target slots) left from a prior visit.
    // Load is rebuilt on entry (see always_dirty) so this shows the
    // watching view.
    app::mock_cancel_load();
    leds::clear_all();
    ui::navigate(Screen::Load);
}
static void on_view(lv_event_t*)      { ui::navigate(Screen::View); }
static void on_pick(lv_event_t*)      { ui::navigate(Screen::PickList); }
static void on_rack(lv_event_t*)      { ui::navigate(Screen::RackGrid); }
static void on_settings(lv_event_t*)  { ui::navigate(Screen::Configure); }

void build_home(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    lv_obj_t* tiles_box = lv_obj_create(body);
    lv_obj_remove_style_all(tiles_box);
    lv_obj_set_size(tiles_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(tiles_box, 1);
    lv_obj_set_layout(tiles_box, LV_LAYOUT_GRID);
    static lv_coord_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_TEMPLATE_LAST };
    static lv_coord_t rows[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(tiles_box, cols, rows);
    lv_obj_set_style_pad_column(tiles_box, 10, 0);
    lv_obj_set_style_pad_row(tiles_box, 10, 0);
    lv_obj_clear_flag(tiles_box, LV_OBJ_FLAG_SCROLLABLE);

    auto place = [tiles_box](lv_obj_t* o, int col, int row, int span = 1) {
        lv_obj_set_grid_cell(o, LV_GRID_ALIGN_STRETCH, col, span,
                                 LV_GRID_ALIGN_STRETCH, row, 1);
    };
    auto mktile = [&](const char* l, const char* s, lv_event_cb_t cb) {
        TileOpts to;
        to.label = l; to.sublabel = s; to.on_click = cb; to.user = nullptr;
        return tile(tiles_box, to);
    };

    // Sublabels stay terse: at 24pt a 3-column tile fits ~2 short words
    // per line before wrapping eats the tile height.
    place(mktile("Load",     "Scan a reel",  on_load),     0, 0);
    place(mktile("View",     "Browse parts", on_view),     1, 0);
    place(mktile("Pick",     "Pick jobs",    on_pick),     2, 0);
    place(mktile("Rack",     "",             on_rack),     0, 1);
    place(mktile("Settings", "Setup",        on_settings), 1, 1, 2);
}

// ---- Rack occupancy screen (the dot grid's new home) ----------------
void build_rack_grid(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    DotGridOpts dg = {};
    dg.show_legend  = true;
    dg.header_label = "Rack overview";
    dg.clickable    = false;
    dot_grid_card(body, dg);
}

} // namespace ui::screens
