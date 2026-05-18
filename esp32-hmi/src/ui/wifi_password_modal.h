// =====================================================================
//  WiFi password entry modal.
//
//  Full-screen overlay on lv_layer_top with an LVGL keyboard wired to
//  a password textarea. Open from the Network screen when the user
//  taps a scan-result row; on Connect the entered password is handed
//  to wifi_mgr::set_credentials() which also persists to SD.
// =====================================================================
#pragma once
#include <stdbool.h>

namespace ui {

void wifi_password_modal_init();    // build once, hidden by default
void wifi_password_modal_open(const char* ssid, bool open_network);
void wifi_password_modal_close();

} // namespace ui
