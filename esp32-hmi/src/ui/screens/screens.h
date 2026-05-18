// =====================================================================
//  Screen builders. Each builder receives the body container and
//  populates it. The screen manager has already cleaned the container
//  before calling.
// =====================================================================
#pragma once
#include <lvgl.h>

namespace ui::screens {

void build_home(lv_obj_t* body);
void build_load(lv_obj_t* body);
void build_view(lv_obj_t* body);
void build_pick_list(lv_obj_t* body);
void build_pick_active(lv_obj_t* body);
void build_configure(lv_obj_t* body);
void build_config_slots(lv_obj_t* body);
void build_config_network(lv_obj_t* body);
void build_config_selftest(lv_obj_t* body);
void build_config_fwupdate(lv_obj_t* body);
void build_config_dividers(lv_obj_t* body);

// Padded, scrollable column for "form" screens. Returns the inner
// container the caller should add form_card()s to.
lv_obj_t* form_scroller(lv_obj_t* body);

// Padded, scrollable column for "row list" screens. Returns the
// inner container.
lv_obj_t* row_scroller(lv_obj_t* body);

} // namespace ui::screens
