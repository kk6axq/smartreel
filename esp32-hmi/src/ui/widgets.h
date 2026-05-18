// =====================================================================
//  Reusable UI widgets that match the HTML mockup primitives.
// =====================================================================
#pragma once

#include <lvgl.h>
#include "ui/app_state.h"

namespace ui {

// ---- Card (rounded white panel) -------------------------------------
lv_obj_t* card(lv_obj_t* parent, int pad = -1);  // -1 = default

// ---- Card head (small uppercase label + thin underline) -------------
lv_obj_t* card_head(lv_obj_t* parent, const char* uppercase_label,
                    const char* right_meta = nullptr);

// ---- Dot grid (4 chains x 16 slots, with chain labels) --------------
// `clickable_states` is a bitmask:
//   bit 0 = TARGET, bit 1 = OCCUPIED, ...; if bits are set the matching
//   dots install `cb` and pass the slot number as user_data.
struct DotGridOpts {
    bool         show_legend     = true;
    const char*  header_label    = "Rack overview";
    bool         clickable       = false;          // if true, all dots are clickable
    void       (*on_dot_click)(int slot_num) = nullptr;
};
lv_obj_t* dot_grid_card(lv_obj_t* parent, const DotGridOpts& opts);

// ---- Row (used by view, pick, anomalies) ---------------------------
// Build empty, fill with helpers below in order. Pure flex row.
struct RowOpts {
    app::SlotState stripe_state = app::SlotState::EMPTY;
    bool           use_accent_stripe = false;       // overrides stripe_state
};
lv_obj_t* row_make(lv_obj_t* parent, const RowOpts& opts = {});

// Row content helpers — append in order matching the HTML mockup.
void row_add_slot_num   (lv_obj_t* row, const char* text);    // "#12" or "BO-0042"
void row_add_main_two_line(lv_obj_t* row,
                           const char* primary, const char* meta);
void row_add_qty_two_line(lv_obj_t* row, const char* num, const char* unit);
lv_obj_t* row_add_button(lv_obj_t* row, const char* text,
                         lv_event_cb_t cb = nullptr, void* user = nullptr,
                         bool muted = false, bool success = false,
                         bool disabled = false);

// ---- Tile (large square button on home / configure menu) -----------
struct TileOpts {
    const char* label;
    const char* sublabel;
    lv_event_cb_t on_click = nullptr;
    void*       user = nullptr;
};
lv_obj_t* tile(lv_obj_t* parent, const TileOpts& opts);

// ---- Menu item (configure sub-grid) --------------------------------
lv_obj_t* menu_item(lv_obj_t* parent, const char* name, const char* desc,
                    lv_event_cb_t cb = nullptr, void* user = nullptr);

// ---- Plain button --------------------------------------------------
enum class BtnKind { Default, Primary, Success, Danger };
lv_obj_t* button(lv_obj_t* parent, const char* text,
                 BtnKind kind = BtnKind::Default,
                 lv_event_cb_t cb = nullptr, void* user = nullptr);

// ---- Form helpers --------------------------------------------------
// Each form_card holds a heading + N form_rows.
lv_obj_t* form_card(lv_obj_t* parent, const char* heading);
lv_obj_t* form_row (lv_obj_t* card_body);

// Build "label / description" left column of a form_row.
void form_row_label(lv_obj_t* row, const char* label, const char* desc = nullptr);

// Right-side input look-alike (read-only-styled label inside a box).
lv_obj_t* form_input(lv_obj_t* row, const char* placeholder_or_value,
                     int width = 200, bool small = false);

// Right-side toggle. `on` is the initial state; the passed pointer is
// updated on every click so callers can read the latest value lazily.
lv_obj_t* form_toggle(lv_obj_t* row, bool on, bool* state_out = nullptr);

// ---- Progress bar (pick-active screen) -----------------------------
lv_obj_t* progress_bar(lv_obj_t* parent, int percent);
void      progress_bar_set(lv_obj_t* bar, int percent);

// ---- Empty-state placeholder ---------------------------------------
lv_obj_t* empty_state(lv_obj_t* parent, const char* text);

} // namespace ui
