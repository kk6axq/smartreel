// =====================================================================
//  Logical slot numbering over the reel topology.
//
//  The Core reports, per PORT (0..3), how many reel MODULES are chained
//  on it (sensed from the rail voltage), and a 32-bit input word per
//  module: 16 reel-presence switches (S bits) interleaved with 16
//  divider-present bits (D bits), as  D0 S0 D1 S1 ... D15 S15
//  (so D_k = bit 2k, S_k = bit 2k+1).
//
//  Numbering model
//  ---------------
//  Every physical slot (port, module, slot) gets a BASE number: compact,
//  1-based, port-ascending, skipping empty ports, then module, then slot
//  (SlotMap, below). Divider bit D_s sits to the RIGHT of slot s (the
//  boundary between slot s and slot s+1); pulling it merges slot s+1 into
//  the run on its left, forming one wider LOGICAL slot. A logical slot's
//  number is the LOWEST base number in its run;
//  absorbed numbers become gaps; neighbours are untouched. Runs may
//  straddle a module boundary within a port, never a port boundary.
//
//  Pure data + computation -- no I/O, no LVGL, no hardware deps.
// =====================================================================
#pragma once

#include <stdint.h>
#include "rs485/rs485_proto.h"

namespace app {

// ---- Base (physical) numbering ------------------------------------
struct SlotMap {
    static constexpr int N_PORTS          = rs485::N_PORTS;             // 4
    static constexpr int SLOTS_PER_MODULE = rs485::MODULE_SLOTS;        // 16
    static constexpr int MAX_MODULES      = rs485::MODULES_PER_PORT_MAX;// 4
    static constexpr int MAX_SLOTS        = N_PORTS * MAX_MODULES * SLOTS_PER_MODULE; // 256

    uint8_t module_count[N_PORTS];
    int     port_slots[N_PORTS];     // module_count * 16
    int     port_start[N_PORTS];     // first base number on the port, 0 if empty
    int     total_slots;

    void rebuild(const uint8_t counts[N_PORTS]);
    bool resolve(int base_num, int& port, int& module, int& slot_in_module) const;
    int  base_of(int port, int module, int slot_in_module) const;
};

// ---- 32-bit module word bit decode --------------------------------
static inline bool slot_present_bit(uint32_t word, int slot) {
    return (word >> (2 * slot + 1)) & 1u;
}
static inline bool divider_present_bit(uint32_t word, int slot) {
    return (word >> (2 * slot)) & 1u;
}

// ---- Divider layout (which dividers are pulled / combined) ---------
// A divider key (port, module, slot) is the divider to the RIGHT of that
// slot -- hardware bit D_slot, the boundary between slot and slot+1. When
// pulled (absent) the slot to its right joins the run on its left. The
// divider past the LAST slot of the LAST module on a port (slot==15) is
// the port edge and is never a combine boundary.
struct DividerLayout {
    static constexpr int MAX_PULLED = 64;
    struct Key { uint8_t port, module, slot; };
    Key pulled[MAX_PULLED];
    int n_pulled = 0;

    void clear() { n_pulled = 0; }
    bool is_pulled(uint8_t port, uint8_t module, uint8_t slot) const;
    void set_pulled(uint8_t port, uint8_t module, uint8_t slot, bool pulled);
};

// ---- A logical slot (possibly combining several physical slots) ----
struct LogicalSlot {
    int     num;      // logical number = lowest base number in the run
    int     width;    // physical slots spanned
    uint8_t port;
    uint8_t module;   // module of the FIRST physical slot in the run
    uint8_t slot;     // slot-in-module of the FIRST physical slot
};

// ---- The built logical rack ---------------------------------------
struct LogicalRack {
    SlotMap      base;
    LogicalSlot  slots[SlotMap::MAX_SLOTS];
    int          n_slots;

    // Build from per-port module counts + a divider layout.
    void rebuild(const uint8_t module_count[SlotMap::N_PORTS],
                 const DividerLayout& div);

    int count() const { return n_slots; }

    // Lookup by logical number (the run's number). nullptr if none.
    const LogicalSlot* by_num(int num) const;

    // The logical slot whose run contains a given physical slot.
    const LogicalSlot* containing(uint8_t port, uint8_t module, uint8_t slot) const;

    // LED pixel span of a logical slot on its port's strip:
    //   pixels [start_pixel, start_pixel + width) where a pixel index is
    //   module*16 + slot. Returns false if num is unknown.
    bool pixel_span(int num, uint8_t& port, int& start_pixel, int& width) const;
};

// ---- Live-vs-committed validation ---------------------------------
enum class DeviationKind : uint8_t { ModuleCount, DividerMissing, DividerExtra };

struct Deviation {
    DeviationKind kind;
    uint8_t       port;
    uint8_t       module;   // for divider deviations
    uint8_t       slot;     // for divider deviations
    uint8_t       expected; // module count (ModuleCount) else 0/1
    uint8_t       actual;
};

// Compare committed config (expected module counts + pulled-divider
// layout) against live hardware (module counts + per-module input words).
// Fills out[0..*n_out-1] (capped at max). A DividerMissing means a
// divider the committed layout expects PRESENT reads absent on the wire.
void validate_topology(const uint8_t committed_counts[SlotMap::N_PORTS],
                       const DividerLayout& committed_div,
                       const uint8_t live_counts[SlotMap::N_PORTS],
                       const uint32_t live_inputs[SlotMap::N_PORTS][SlotMap::MAX_MODULES],
                       Deviation* out, int max, int* n_out);

} // namespace app
