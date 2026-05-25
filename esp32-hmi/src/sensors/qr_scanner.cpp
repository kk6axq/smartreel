#include "sensors/qr_scanner.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp32-hal-log.h>
#include <string.h>

namespace qr_scanner {

static bool s_present = false;

bool init() {
    Wire.beginTransmission(I2C_ADDR);
    s_present = (Wire.endTransmission() == 0);
    if (s_present) {
        log_i("QR scanner detected at 0x%02X", I2C_ADDR);
    } else {
        log_i("QR scanner not present at 0x%02X", I2C_ADDR);
    }
    return s_present;
}

bool present() { return s_present; }

bool poll(char* out, size_t out_cap) {
    if (!s_present || !out || out_cap < 2) return false;

    // Tiny Code Reader returns a fixed 256-byte struct: 2-byte length
    // (LE) + 254-byte content. Reading any other size confuses the
    // driver on the device.
    constexpr size_t READ_LEN = 256;
    uint8_t buf[READ_LEN];
    size_t got = Wire.requestFrom((int)I2C_ADDR, (int)READ_LEN);
    if (got != READ_LEN) {
        // The device usually NAKs the address briefly when busy.
        // Treat as transient; the caller can poll again.
        // Drain whatever did arrive.
        while (Wire.available()) (void)Wire.read();
        return false;
    }
    for (size_t i = 0; i < READ_LEN; ++i) buf[i] = Wire.read();

    uint16_t content_len = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (content_len == 0 || content_len > MAX_LEN) return false;

    size_t copy = (content_len < out_cap - 1) ? content_len : (out_cap - 1);
    memcpy(out, &buf[2], copy);
    out[copy] = '\0';
    return true;
}

} // namespace qr_scanner
