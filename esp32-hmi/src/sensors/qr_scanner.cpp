#include "sensors/qr_scanner.h"
#include "board/i2c_bus.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp32-hal-log.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace qr_scanner {

// =====================================================================
//  The I2C read used to run in an lv_timer on the LVGL thread -- a
//  256-byte blocking transfer (several ms) every 200 ms, stalling the
//  render loop (review item B). Now a worker task pinned to APP_CPU owns
//  all I2C traffic to the reader; it latches the latest decoded code (and
//  live presence) under a lock. poll()/present() just read that latch and
//  never touch the bus, so the LVGL thread never blocks on the scanner.
//
//  poll() keeps its free-running contract: while a code is held in view
//  it returns that code on every call. Screen-side dedupe (load.cpp's
//  g_last_resolve_qr) is unchanged.
// =====================================================================

static constexpr int POLL_MS = 200;          // 5 Hz, within the device cap

static volatile bool     s_present = false;
static SemaphoreHandle_t s_mtx     = nullptr;   // guards s_code / s_have
static TaskHandle_t      s_worker  = nullptr;
static char              s_code[MAX_LEN + 1] = {0};
static bool              s_have    = false;

// One blocking device read, performed on the worker thread under the I2C
// lock. Returns true and fills s_code/s_have via the caller's snapshot.
static bool read_device(char* out, size_t out_cap) {
    constexpr size_t READ_LEN = 256;
    uint8_t buf[READ_LEN];
    {
        i2c_bus::Lock _g;
        size_t got = Wire.requestFrom((int)I2C_ADDR, (int)READ_LEN);
        if (got != READ_LEN) {
            while (Wire.available()) (void)Wire.read();   // drain partial
            return false;
        }
        for (size_t i = 0; i < READ_LEN; ++i) buf[i] = Wire.read();
    }
    uint16_t content_len = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (content_len == 0 || content_len > MAX_LEN) return false;
    size_t copy = (content_len < out_cap - 1) ? content_len : (out_cap - 1);
    memcpy(out, &buf[2], copy);
    out[copy] = '\0';
    return true;
}

static bool probe_present() {
    i2c_bus::Lock _g;
    Wire.beginTransmission(I2C_ADDR);
    return Wire.endTransmission() == 0;
}

static void scanner_worker(void* /*arg*/) {
    char latest[MAX_LEN + 1];
    for (;;) {
        const bool present = probe_present();
        s_present = present;
        bool have = false;
        if (present) have = read_device(latest, sizeof(latest));

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_have = have;
        if (have) { strncpy(s_code, latest, sizeof(s_code) - 1); s_code[sizeof(s_code) - 1] = 0; }
        xSemaphoreGive(s_mtx);

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

bool init() {
    // First presence probe is synchronous (still single-threaded here).
    s_present = probe_present();
    if (s_present) log_i("QR scanner detected at 0x%02X", I2C_ADDR);
    else           log_i("QR scanner not present at 0x%02X", I2C_ADDR);

    if (!s_worker) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx) {
            // APP_CPU, priority 1 (below the LVGL task) so it never preempts
            // rendering; the I2C lock serialises it against touch reads.
            xTaskCreatePinnedToCore(scanner_worker, "qr", 4096, nullptr, 1,
                                    &s_worker, APP_CPU_NUM);
        }
    }
    return s_present;
}

bool present() { return s_present; }

bool poll(char* out, size_t out_cap) {
    if (!out || out_cap < 2 || !s_mtx) return false;
    bool have = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_have) {
        size_t copy = strnlen(s_code, MAX_LEN);
        if (copy > out_cap - 1) copy = out_cap - 1;
        memcpy(out, s_code, copy);
        out[copy] = '\0';
        have = true;
    }
    xSemaphoreGive(s_mtx);
    return have;
}

} // namespace qr_scanner
