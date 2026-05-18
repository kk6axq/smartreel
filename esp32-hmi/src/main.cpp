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
//    9) RS485 master        -- UART1 in half-duplex mode; starts the
//                              background POLL task that pulls async
//                              events off the Core PCB.
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
#include "rs485/rs485.h"

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

// ===================================================================
// RS485 async event handler.
//
// The POLL task in rs485.cpp calls this from its own thread. Keep
// it short -- any UI work has to be marshalled onto the LVGL task,
// which we'll do once the relevant UI hooks are wired. For now we
// just log so we can verify the wire path is working.
// ===================================================================
static void on_rs485_event(const rs485::Event& e, void* /*user*/) {
    switch (e.type) {
        case rs485::EVT_INPUT_CHANGE:
            if (e.payload_len >= 7) {
                const uint8_t reel    = e.payload[0];
                const uint8_t prev    = e.payload[1];
                const uint8_t now     = e.payload[2];
                const uint32_t ts_ms  = ((uint32_t)e.payload[3] << 24) |
                                         ((uint32_t)e.payload[4] << 16) |
                                         ((uint32_t)e.payload[5] <<  8) |
                                         ((uint32_t)e.payload[6] <<  0);
                log_i("rs485 INPUT_CHANGE reel=%u 0x%02X->0x%02X t=%lu",
                      reel, prev, now, (unsigned long)ts_ms);
            }
            break;

        case rs485::EVT_REEL_INSERTED:
            if (e.payload_len >= 3) {
                const uint8_t reel    = e.payload[0];
                const uint16_t mv     = ((uint16_t)e.payload[1] << 8) | e.payload[2];
                log_i("rs485 REEL_INSERTED reel=%u sense=%u mV", reel, mv);
            }
            break;

        case rs485::EVT_REEL_REMOVED:
            if (e.payload_len >= 1) {
                log_i("rs485 REEL_REMOVED reel=%u", e.payload[0]);
            }
            break;

        case rs485::EVT_SENSE_THRESHOLD:
            log_i("rs485 SENSE_THRESHOLD evt (%u bytes)", e.payload_len);
            break;

        case rs485::EVT_LOG:
            if (e.payload_len >= 2) {
                // payload: level (1B), ASCII message
                char buf[160];
                size_t n = e.payload_len - 1;
                if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                memcpy(buf, &e.payload[1], n);
                buf[n] = 0;
                log_i("[core L%u] %s", e.payload[0], buf);
            }
            break;

        default:
            log_w("rs485 unknown event 0x%02X (%u bytes)", e.type, e.payload_len);
            break;
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

    // 9) RS485 master + POLL task
    {
        rs485::set_event_handler(on_rs485_event, nullptr);
        rs485::Status rs = rs485::init();
        if (rs != rs485::Status::Ok) {
            Serial.printf("[boot] rs485 init failed: %s\n", rs485::status_str(rs));
        } else {
            Serial.println("[boot] rs485 ready (polling Core)");
        }
    }

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
