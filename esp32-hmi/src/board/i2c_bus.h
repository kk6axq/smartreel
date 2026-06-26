// =====================================================================
//  Shared I2C bus lock.
//
//  Touch (GT911), the CH422G IO expander, and the QR scanner all hang
//  off the single Wire bus. They used to be safe without a lock only
//  because every access ran on the one LVGL thread. Once the scanner
//  moved to its own worker task (review item B), two threads can issue
//  Wire transactions concurrently -- which corrupts the bus (especially
//  GT911 reads, which use a repeated-START that must not be interrupted).
//
//  Every complete logical I2C transaction must be wrapped in a Lock.
//  Lock with the mutex uncreated is a harmless no-op, so early single-
//  threaded boot access (before init()) needs no special-casing.
// =====================================================================
#pragma once

namespace i2c_bus {

// Create the mutex. Call once in setup(), right after Wire.begin() and
// before any worker that touches I2C is started.
void init();

// RAII guard: holds the bus for one transaction.
struct Lock {
    Lock();
    ~Lock();
    Lock(const Lock&)            = delete;
    Lock& operator=(const Lock&) = delete;
};

} // namespace i2c_bus
