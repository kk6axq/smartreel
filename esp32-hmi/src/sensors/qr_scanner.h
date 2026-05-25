// =====================================================================
//  Useful Sensors "Tiny Code Reader"
//
//  Small camera module on the I2C bus that decodes QR codes on-device
//  and returns the latest decoded text via I2C reads. The chip caches
//  the most recent scan and clears the "fresh" flag once read, so
//  polling at ~5 Hz gives us each new code exactly once.
//
//  Wire format on a read of 256 bytes:
//      byte 0..1 : content_length (uint16 little-endian, 0 if no
//                  fresh scan since the last read)
//      byte 2..  : decoded UTF-8 (URL or arbitrary string)
//
//  I2C address: 0x0C (fixed by the module).
// =====================================================================
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

namespace qr_scanner {

constexpr uint8_t I2C_ADDR = 0x0C;
constexpr size_t  MAX_LEN  = 254;        // payload cap per datasheet

// Probe the I2C bus. Sets present() == true if the device ACKs.
// Safe to re-call (treats as a re-detect).
bool init();

// Last-known device presence (no I2C traffic).
bool present();

// Polled read. If a fresh scan is buffered on the device, copies the
// decoded UTF-8 (null-terminated) into `out` and returns true. If
// no fresh scan is available, the device responded but the buffer is
// empty, or the I2C transaction failed, returns false and leaves
// `out` untouched.
//
// `out_cap` must be >= 2; longer content is truncated to fit and
// still null-terminated.
bool poll(char* out, size_t out_cap);

} // namespace qr_scanner
