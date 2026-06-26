// =====================================================================
//  App-layer LED control. Thin wrappers around rs485:: that know
//  about the slot <-> reel/pixel mapping and use the COMMIT pattern
//  to make multi-reel updates land atomically.
//
//  Slot numbering: 1..N_SLOTS. slot 1..16 are chain 1 (reel_id 0),
//  17..32 are chain 2 (reel_id 1), etc. Within a chain, position
//  1..16 maps to pixel index 0..15.
//
//  All functions are safe to call when no Core PCB is present: the
//  underlying RS485 transactions just time out and return
//  Status::Timeout. UI handlers can fire and forget.
// =====================================================================
#pragma once

#include "rs485/rs485.h"
#include <stdint.h>

namespace leds {

// Start the async LED worker (APP_CPU). Call once at boot, after the reel
// topology is known and boot_chase() has finished. Until it runs, the
// light_*/fill/clear calls below are harmless no-ops. The actual RS485
// push happens on this worker, never on the caller's (LVGL) thread.
void start();

// Light one slot. Sends a single-pixel set + commit for that reel.
rs485::Status light_slot(int slot_num, uint8_t r, uint8_t g, uint8_t b);

// Light every slot whose state == TARGET (per app::state()). Used
// at pick-start and load-placed to surface the targets on the rack.
// Default colour is theme blue (accent).
rs485::Status light_target_slots(uint8_t r = 0x25, uint8_t g = 0x63, uint8_t b = 0xEB);

// Light every slot whose state == EMPTY. Used at load-scan to show
// available placement targets. Default colour is theme green.
rs485::Status light_empty_slots(uint8_t r = 0x16, uint8_t g = 0xA3, uint8_t b = 0x4A);

// Fill every connected reel with a flat colour + commit. Used by
// self-test "All LEDs run".
rs485::Status fill_all(uint8_t r, uint8_t g, uint8_t b);

// Turn off every LED on every reel.
rs485::Status clear_all();

// Boot self-test: chase a single lit pixel down the line through every
// connected reel module (port by port, pixel by pixel), then turn all
// LEDs off. Blocking (uses delay); call from the main/setup thread
// before the LVGL task starts, after the reel topology is known.
// No-ops harmlessly when no Core/modules are present.
void boot_chase();

} // namespace leds
