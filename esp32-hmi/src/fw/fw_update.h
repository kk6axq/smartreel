// =====================================================================
//  Firmware update -- HMI self-OTA from the SD card.
//
//  The ESP32 partition table carries two OTA app slots (app0/app1).
//  update_hmi_from_sd() streams an image file off the SD card into the
//  *inactive* slot via the Arduino Update library, validates it, and
//  marks it as the next boot target. The caller reboots to run it.
//
//  Images are dropped onto the SD card manually for now; a WiFi/HTTP
//  push is the planned long-term source (see docs/roadmap.md).
//
//  The RP2040-over-RS485 update path lives in fw_core_update.*.
// =====================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace fw {

// Progress callback: (bytes_done, bytes_total). May be nullptr.
typedef void (*ProgressCb)(size_t done, size_t total);

enum class Result : int8_t {
    Ok            =  0,
    NoCard        = -1,   // SD not mounted
    NoFile        = -2,   // image file missing / unreadable
    TooBig        = -3,   // image larger than the target OTA slot
    BeginFailed   = -4,   // Update.begin() failed
    WriteFailed   = -5,   // a chunk write failed
    EndFailed     = -6,   // finalize/verify failed (bad image)
    ReadError     = -7,   // SD read error mid-stream
};
const char* result_str(Result r);

// Stream "/sdcard/<filename>" into the inactive OTA slot and set it as
// the boot partition. Returns Ok on success; on Ok the caller should
// reboot (ESP.restart()) to run the new image. On any error the running
// app is untouched.
Result update_hmi_from_sd(const char* filename, ProgressCb cb = nullptr);

// Label of the currently-running OTA partition (e.g. "app0"/"app1").
const char* running_partition_label();

// Project/app version string baked into the running image (from the
// app descriptor). Useful to confirm an update actually took.
const char* running_app_version();

} // namespace fw
