// =====================================================================
//  Reel Rack HMI -- entry point.
//
//  Bring-up order matters:
//    1) Wire (I2C)         -- so the CH422G expander can answer.
//    2) CH422G              -- everything below it (LCD, touch) needs
//                              the expander's reset / backlight pins.
//    3) RGB display + LVGL  -- the framebuffer is the heaviest user
//                              of PSRAM, do this before allocating
//                              any large UI structures.
//    4) GT911 touch         -- registers an LVGL input device.
//    5) SD card             -- best-effort; the UI shows a "NO SD"
//                              warning pill if the card is missing.
//    6) Config store        -- reads /sdcard/config.json + rack.json,
//                              applies to app_state.
//    7) UI                  -- theme, screen manager.
//    8) WiFi manager        -- non-blocking; starts a connection
//                              attempt if credentials are present.
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "board/board_pins.h"
#include "board/ch422g.h"
#include "display/display.h"
#include "touch/gt911.h"
#include "storage/sdcard.h"
#include "storage/config_store.h"
#include "net/wifi_mgr.h"

#include "ui/theme.h"
#include "ui/app_state.h"
#include "ui/status_bar.h"
#include "ui/screen_manager.h"

static void lvgl_task(void* /*arg*/) {
    constexpr TickType_t period = pdMS_TO_TICKS(5);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        lv_timer_handler();
        vTaskDelayUntil(&last, period);
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[boot] reel-rack HMI starting");

    // 1) I2C bus
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);

    // 2) Expander
    if (!ch422g::init()) {
        Serial.println("[boot] CH422G init FAILED");
    }
    delay(20);

    // 3) LVGL core + display
    lv_init();
    if (!display::init()) {
        Serial.println("[boot] display init FAILED -- halting");
        for (;;) { delay(1000); }
    }

    // 4) Touch
    if (!touch::init()) {
        Serial.println("[boot] touch init failed (display still works)");
    }

    // 5) SD card (optional)
    bool sd_ok = sdcard::init();
    if (!sd_ok) {
        Serial.println("[boot] SD missing -- running on mock data");
    }

    // 6) Config store: read JSON from SD if available, otherwise use
    //    defaults. apply_to_app_state only overlays non-empty values
    //    so the UI's mock data stays intact when no SD card.
    config_store::init();
    (void)app::state();           // seed mock data
    config_store::apply_to_app_state();

    // 7) UI
    theme::init();
    ui::init();
    ui::status_bar_set_sd_missing(!sd_ok);
    Serial.println("[boot] UI ready");

    // 8) WiFi (non-blocking; events drive app_state.online + status bar)
    wifi_mgr::init();

    // LVGL runs on its own task pinned to APP_CPU so the RGB DMA
    // refresh on PRO_CPU isn't disrupted.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8 * 1024, nullptr,
                            2, nullptr, APP_CPU_NUM);
}

void loop() {
    // Reserved for application-level work (Inventree poll, RS485 IO,
    // anomaly detection). LVGL runs on its own task, see setup().
    delay(100);
}
