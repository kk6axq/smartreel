// =====================================================================
//  microSD card -- SDMMC 1-bit mode.
//
//  The Waveshare 4.3B routes its microSD slot to the ESP32-S3's
//  SD/MMC peripheral, not to SPI. We mount in 1-bit mode (CMD/CLK/D0)
//  which uses three GPIOs and keeps the rest free for the LCD.
//
//  Mount point is "/sdcard". Use sdcard::mounted() to check before
//  reading; the system runs fine without an SD card (config falls
//  back to compiled-in defaults).
// =====================================================================
#pragma once
#include <stdbool.h>
#include <stddef.h>

namespace sdcard {

// Mount the card. Idempotent. Returns true if a card is present and
// readable.
bool init();

// True after a successful init().
bool mounted();

// Path on the filesystem (mount point). Stable across re-mounts.
const char* mount_point();

// File-size helper - returns 0 if not mounted or file missing.
size_t file_size(const char* path);

// Read whole file into the supplied buffer. Returns bytes read, or
// 0 on failure. The buffer is NOT null-terminated; if you need a
// C-string append your own '\0' (but make sure buf_len > file_size).
size_t read_file(const char* path, void* buf, size_t buf_len);

// Atomic write: writes to "<path>.tmp" then renames over the target
// so a power loss can never leave a half-written config. Returns
// true on success.
bool write_file_atomic(const char* path, const void* buf, size_t len);

// Card capacity in bytes (0 if not mounted).
unsigned long long capacity_bytes();

// Wipe the card and re-format as FAT32. Destructive, NO confirm
// here -- the UI is responsible for asking the user. Returns true
// on success. After format the card is left mounted.
bool format();

} // namespace sdcard
