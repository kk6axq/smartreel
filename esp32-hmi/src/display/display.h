// =====================================================================
//  RGB LCD bring-up and LVGL display driver registration.
// =====================================================================
#pragma once

#include <stdbool.h>
#include <lvgl.h>

namespace display {

// Bring up the panel (resets, backlight) and register an LVGL display.
// Must be called AFTER ch422g::init() and Wire.begin().
// Returns false if the esp_lcd panel could not be created.
bool init();

// LVGL flush callback - exposed only for unit testing.
void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px);

// Convenience: returns the LVGL display object created by init().
lv_disp_t* lvgl_display();

} // namespace display
