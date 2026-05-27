// =====================================================================
//  Firmware update -- push a new RP2040 Core image over RS485.
//
//  Reads a Core firmware .bin off the SD card, then drives the FW_*
//  protocol (docs/smartreel-rs485-protocol.md) against the Core:
//    FW_BEGIN (size + SHA-256 + version) -> FW_CHUNK xN -> FW_VERIFY
//    -> FW_COMMIT. The Core stages the image via arduino-pico OTA and
//    reboots into it.
//
//  The background POLL task is paused for the duration so the bus is
//  exclusive to the transfer.
// =====================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace fw {

typedef void (*ProgressCb)(size_t done, size_t total);

enum class CoreResult : int8_t {
    Ok           =  0,
    NoCard       = -1,
    NoFile       = -2,
    Oom          = -3,   // couldn't allocate the image buffer
    BeginFailed  = -4,   // Core rejected/Did not ack FW_BEGIN
    ChunkFailed  = -5,   // a chunk failed after retries
    VerifyFailed = -6,   // Core hash mismatch
    CommitFailed = -7,   // Core rejected FW_COMMIT
};
const char* core_result_str(CoreResult r);

// Push "/sdcard/<filename>" to the Core over RS485. On Ok the Core has
// committed and is rebooting into the new image.
CoreResult update_core_from_sd(const char* filename, ProgressCb cb = nullptr);

} // namespace fw
