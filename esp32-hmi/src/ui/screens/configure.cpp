// CONFIGURE -- 3x2 menu grid.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"

namespace ui::screens {

static void on_slots(lv_event_t*)    { ui::navigate(Screen::ConfigSlots); }
static void on_network(lv_event_t*)  { ui::navigate(Screen::ConfigNetwork); }
static void on_selftest(lv_event_t*) { ui::navigate(Screen::ConfigSelftest); }
static void on_fwupdate(lv_event_t*) { ui::navigate(Screen::ConfigFwupdate); }
static void on_dividers(lv_event_t*) { ui::navigate(Screen::ConfigDividers); }

void build_configure(lv_obj_t* body) {
    lv_obj_set_layout(body, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_column(body, 10, 0);
    lv_obj_set_style_pad_row(body, 10, 0);

    static lv_coord_t cols[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t rows[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(body, cols, rows);

    auto place = [body](lv_obj_t* o, int col, int row) {
        lv_obj_set_grid_cell(o, LV_GRID_ALIGN_STRETCH, col, 1,
                                 LV_GRID_ALIGN_STRETCH, row, 1);
    };
    // No "Exit" tile (review item R14): the status-bar back button already
    // returns Home.
    place(menu_item(body, "Slots",    "Chains, slot count and numbering",      on_slots),    0, 0);
    place(menu_item(body, "Network",  "Wi-Fi and Inventree server",            on_network),  1, 0);
    place(menu_item(body, "Self Test","LEDs, buttons, anomalies",              on_selftest), 2, 0);
    place(menu_item(body, "Firmware", "Check & install updates",               on_fwupdate), 0, 1);
    place(menu_item(body, "Dividers", "Add or remove dividers",                on_dividers), 1, 1);
}

} // namespace ui::screens
