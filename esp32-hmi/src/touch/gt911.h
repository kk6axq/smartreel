// =====================================================================
//  Goodix GT911 capacitive touch driver -- minimal, polled.
//
//  We register an LVGL indev_drv so the rest of the project just sees
//  LV_INDEV_TYPE_POINTER. Multi-touch is not exposed (LVGL pointer
//  driver is single-touch only).
// =====================================================================
#pragma once

#include <lvgl.h>
#include <stdbool.h>

namespace touch {

// Bring the GT911 out of reset, probe its address, and register an
// LVGL input device. Returns false if the chip never responds.
bool init();

void read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);

} // namespace touch
