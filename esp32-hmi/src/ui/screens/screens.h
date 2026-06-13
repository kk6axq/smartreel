// =====================================================================
//  Screen builders. Each builder receives the body container and
//  populates it. The screen manager has already cleaned the container
//  before calling.
// =====================================================================
#pragma once
#include <lvgl.h>
#include <stdint.h>

namespace ui::screens {

void build_home(lv_obj_t* body);
void build_load(lv_obj_t* body);
void build_view(lv_obj_t* body);
void build_rack_grid(lv_obj_t* body);
void build_pick_list(lv_obj_t* body);
void build_pick_active(lv_obj_t* body);
void build_configure(lv_obj_t* body);
void build_config_slots(lv_obj_t* body);
void build_config_network(lv_obj_t* body);
void build_config_selftest(lv_obj_t* body);
void build_config_fwupdate(lv_obj_t* body);
void build_config_dividers(lv_obj_t* body);
void build_qr_scanner(lv_obj_t* body);

// Padded, scrollable column for "form" screens. Returns the inner
// container the caller should add form_card()s to.
lv_obj_t* form_scroller(lv_obj_t* body);

// Padded, scrollable column for "row list" screens. Returns the
// inner container.
lv_obj_t* row_scroller(lv_obj_t* body);

// Self Test "Buttons" card: live input observation. main.cpp routes
// raw module-word changes here (and suppresses the workflow/anomaly
// modals) while the Self Test screen is current, so a reel button or
// divider can be pressed and its decoded slot read off the screen.
// No-op if the Buttons card isn't currently built.
void selftest_on_input(uint8_t port, uint8_t module,
                       uint32_t prev, uint32_t now);

} // namespace ui::screens
