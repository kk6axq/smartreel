// =====================================================================
//  Live hardware mirror -- the HMI's view of what the Core sees.
//
//  Holds the current per-port module count and the latest 32-bit input
//  word per module (reel-presence S bits + divider D bits). It is the
//  single source the logical rack, the divider edit screen, and the
//  topology validator read from.
//
//  Threading: owned by the LVGL task. The RS485 POLL task delivers
//  changes via ui::dispatch_on_lvgl, so all reads and writes happen on
//  the LVGL thread -- no lock needed (same single-writer convention as
//  app_state's UI side). sync_from_core() issues blocking RS485 reads
//  and so must only be called from the LVGL/main thread (at boot or on a
//  topology change), never from the POLL task.
// =====================================================================
#pragma once

#include <stdint.h>
#include "app/slot_map.h"

namespace hw_mirror {

constexpr int N_PORTS     = app::SlotMap::N_PORTS;
constexpr int MAX_MODULES = app::SlotMap::MAX_MODULES;
constexpr int SLOTS       = app::SlotMap::SLOTS_PER_MODULE;

void     reset();
bool     valid();

uint8_t  module_count(int port);
void     set_module_count(int port, uint8_t count);

uint32_t inputs(int port, int module);
void     set_inputs(int port, int module, uint32_t word);

bool     slot_present(int port, int module, int slot);     // S bit
bool     divider_present(int port, int module, int slot);  // D bit

// Raw arrays for the engine rebuild / validator.
const uint8_t*  module_counts();                                   // [N_PORTS]
const uint32_t (*all_inputs())[MAX_MODULES];                       // [N_PORTS][MAX_MODULES]

// Pull module counts (from the cached reel info) and the per-module
// input words (blocking rs485::read_inputs) from the Core. Marks the
// mirror valid on success. LVGL/main thread only.
bool sync_from_core();

} // namespace hw_mirror
