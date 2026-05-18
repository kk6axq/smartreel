// =====================================================================
//  Persistent status bar.
//
//  Lives on lv_layer_top() so it stays visible across screen swaps.
//  Owns the back button, current-screen title, the "ONLINE" pill, and
//  the anomaly badge.
//
//  Layout (left to right):
//      [back-btn 32x32]   [title (centered, flex 1)]   [pill] [badge]
//
//  The back button is hidden on the home screen.
// =====================================================================
#pragma once

#include <lvgl.h>

namespace ui {

void status_bar_init();

// Update visible bits when navigation state changes. The screen
// manager calls this after every navigate().
void status_bar_set_title(const char* text);
void status_bar_set_back_visible(bool visible);
void status_bar_set_online(bool online);
void status_bar_set_anomaly_count(int count);

// Show / hide the orange "NO SD - MOCK DATA" pill that warns the
// operator when no SD card is present and we're running on built-in
// mock data.
void status_bar_set_sd_missing(bool missing);

} // namespace ui
