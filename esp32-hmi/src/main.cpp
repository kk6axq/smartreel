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
#include "storage/state_store.h"
#include "net/wifi_mgr.h"
#include "rs485/rs485.h"
#include "sensors/qr_scanner.h"
#include "util/lvgl_async.h"
#include "ui/anomaly_modal.h"
#include "ui/status_bar.h"

#include <new>
#include <string.h>

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
// RS485 async event dispatcher.
//
// The RS485 POLL task calls on_rs485_event from its own thread. Per
// our threading model (LVGL is the sole writer of app_state), all
// state mutations have to happen on the LVGL task. We parse the
// event payload here in the POLL task, allocate a tiny effect-
// descriptor struct on the heap, and ui::dispatch_on_lvgl it; the
// LVGL-task callback then mutates app_state, persists, and surfaces
// any UI side effects (anomaly modal, etc.).
// ===================================================================

// Heap-passed payload from POLL task -> LVGL task for input changes.
struct InputChangeEvent {
    uint8_t  chain;       // 1..4 (reel_id + 1)
    uint16_t prev_bits;   // bit N == presence of slot at position N+1
    uint16_t new_bits;
    uint32_t ts_ms;
};

// Apply one slot's transition. Runs on the LVGL task; takes the
// app_state lock for its mutations.
static void apply_slot_change(int slot_num, bool now_present) {
    auto& st = app::state();
    app::Slot* slot = app::slot_by_num(slot_num);
    if (!slot) return;

    // Was this change part of an active workflow's expectation?
    bool expected = false;

    // Pick workflow: removing a reel from a target slot completes
    // that line item.
    if (!now_present && st.active_pick_idx >= 0
        && slot->state == app::SlotState::TARGET) {
        auto& job = st.pick_jobs[st.active_pick_idx];
        for (int i = 0; i < job.n_items; ++i) {
            if (job.items[i].slot_num == slot_num && !job.items[i].picked) {
                app::lock();
                job.items[i].picked = true;
                slot->state = app::SlotState::PICKED;
                app::unlock();
                state_store::mark_jobs_dirty();
                state_store::mark_slot_dirty(slot_num);
                expected = true;
                break;
            }
        }
    }

    // Load workflow: inserting a reel into a target slot commits
    // the load. mock_place_reel does the heavy lifting (clears
    // other lit, assigns part, marks dirty).
    if (now_present && st.load_step == app::LoadStep::Placed
        && slot->state == app::SlotState::TARGET) {
        app::mock_place_reel(slot_num);
        expected = true;
    }

    if (expected) return;

    // Unexpected: update slot state to reflect the anomaly and raise
    // the modal. The anomaly content is auto-populated by
    // mock_raise_anomaly using the first matching slot; for Phase 1
    // that's good enough.
    app::lock();
    slot->state = now_present ? app::SlotState::WARN
                              : app::SlotState::ERROR;
    app::unlock();
    state_store::mark_slot_dirty(slot_num);

    ui::anomaly_modal_raise(now_present ? app::AnomalyKind::Added
                                        : app::AnomalyKind::Removed);
}

static void apply_input_change(void* user) {
    auto* ev = static_cast<InputChangeEvent*>(user);
    if (!ev) return;

    const uint16_t changed = ev->prev_bits ^ ev->new_bits;
    for (int bit = 0; bit < 16; ++bit) {
        if (!(changed & (1u << bit))) continue;
        const int position = bit + 1;        // 1..16 within chain
        const app::Slot* s = app::slot_at(ev->chain, position);
        if (!s) continue;
        const bool now_present = (ev->new_bits & (1u << bit)) != 0;
        apply_slot_change(s->slot, now_present);
    }
    delete ev;
}

static void on_rs485_event(const rs485::Event& e, void* /*user*/) {
    switch (e.type) {
        case rs485::EVT_INPUT_CHANGE: {
            // Two payload shapes (per docs/smartreel-rs485-protocol.md):
            //   9 B: reel_id(1) + prev(2 BE) + new(2 BE) + ts(4 BE)
            //        -- 16 inputs per reel (two chained 74HC165s)
            //   7 B: reel_id(1) + prev(1) + new(1) + ts(4 BE)
            //        -- 8 inputs per reel (single PISO)
            uint8_t  chain = 0;
            uint16_t prev  = 0;
            uint16_t now   = 0;
            uint32_t ts    = 0;
            if (e.payload_len == 9) {
                chain = e.payload[0] + 1;
                prev  = ((uint16_t)e.payload[1] << 8) | e.payload[2];
                now   = ((uint16_t)e.payload[3] << 8) | e.payload[4];
                ts    = ((uint32_t)e.payload[5] << 24) |
                        ((uint32_t)e.payload[6] << 16) |
                        ((uint32_t)e.payload[7] <<  8) |
                        ((uint32_t)e.payload[8] <<  0);
            } else if (e.payload_len == 7) {
                chain = e.payload[0] + 1;
                prev  = e.payload[1];
                now   = e.payload[2];
                ts    = ((uint32_t)e.payload[3] << 24) |
                        ((uint32_t)e.payload[4] << 16) |
                        ((uint32_t)e.payload[5] <<  8) |
                        ((uint32_t)e.payload[6] <<  0);
            } else {
                log_w("rs485 INPUT_CHANGE: unexpected payload_len=%u",
                      e.payload_len);
                break;
            }
            (void)ts;
            auto* ev = new (std::nothrow) InputChangeEvent{ chain, prev, now, ts };
            if (ev) ui::dispatch_on_lvgl(apply_input_change, ev);
            break;
        }

        case rs485::EVT_REEL_INSERTED:
            // For Phase 1b we just log. Per-chain presence tracking
            // and "whole chain disappeared" anomaly is a future
            // enhancement.
            if (e.payload_len >= 3) {
                uint8_t  reel = e.payload[0];
                uint16_t mv   = ((uint16_t)e.payload[1] << 8) | e.payload[2];
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

    // 4b) QR scanner (optional, on the same I2C bus as touch + CH422G)
    if (!qr_scanner::init()) {
        Serial.println("[boot] QR scanner not present at 0x0C (re-probe from Self Test)");
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

    // 6b) Runtime-state store: load persisted rack + pick queue from
    //     SD over the mock seed. Returns false if SD missing or files
    //     don't exist yet (first boot) -- the mock seed stays. The
    //     writer task takes over from here for any saves triggered
    //     by mutations.
    bool state_loaded = state_store::load();
    app::set_boot_loaded_from_sd(state_loaded);
    if (!state_store::start_writer()) {
        Serial.println("[boot] state_store writer task failed to start");
    }

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
