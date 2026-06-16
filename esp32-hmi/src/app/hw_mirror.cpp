// =====================================================================
//  Live hardware mirror (see hw_mirror.h).
// =====================================================================
#include "app/hw_mirror.h"
#include "rs485/rs485.h"

#include <Arduino.h>
#include <string.h>

namespace hw_mirror {

static uint8_t  s_module_count[N_PORTS]              = { 0 };
static uint32_t s_inputs[N_PORTS][MAX_MODULES]       = {{ 0 }};
static bool     s_valid                              = false;

void reset() {
    memset(s_module_count, 0, sizeof(s_module_count));
    memset(s_inputs, 0, sizeof(s_inputs));
    s_valid = false;
}

bool valid() { return s_valid; }

uint8_t module_count(int port) {
    return (port >= 0 && port < N_PORTS) ? s_module_count[port] : 0;
}
void set_module_count(int port, uint8_t count) {
    if (port < 0 || port >= N_PORTS) return;
    if (count > MAX_MODULES) count = MAX_MODULES;
    s_module_count[port] = count;
    // Words for modules that are no longer present are stale; clear them
    // so a re-inserted module starts from a clean (all-released) baseline.
    for (int m = count; m < MAX_MODULES; ++m) s_inputs[port][m] = 0;
}

uint32_t inputs(int port, int module) {
    if (port < 0 || port >= N_PORTS || module < 0 || module >= MAX_MODULES) return 0;
    return s_inputs[port][module];
}
void set_inputs(int port, int module, uint32_t word) {
    if (port < 0 || port >= N_PORTS || module < 0 || module >= MAX_MODULES) return;
    s_inputs[port][module] = word;
}

bool slot_present(int port, int module, int slot) {
    return app::slot_present_bit(inputs(port, module), slot);
}
bool divider_present(int port, int module, int slot) {
    return app::divider_present_bit(inputs(port, module), slot);
}

const uint8_t* module_counts() { return s_module_count; }
const uint32_t (*all_inputs())[MAX_MODULES] { return s_inputs; }

bool sync_from_core() {
    // Module counts via a direct read (the cached copy may not be primed
    // yet at boot, and modules already present when both boards power up
    // never emit an insert event -- so we must read explicitly).
    rs485::ReelInfo ri[N_PORTS];
    int n = 0;
    if (rs485::get_reel_info(rs485::REEL_ID_ALL, ri, N_PORTS, &n) != rs485::Status::Ok)
        return false;

    uint8_t counts[N_PORTS] = { 0 };
    for (int i = 0; i < n; ++i)
        if (ri[i].port < N_PORTS) counts[ri[i].port] = ri[i].module_count;
    for (int p = 0; p < N_PORTS; ++p) set_module_count(p, counts[p]);

    // Per-module input words via a blocking read of each present port.
    //
    // At boot the Core may not have scanned its 74HC165 chains yet, so the
    // first read can come back all-zero -- which the rack engine reads as
    // "every divider pulled" and collapses a whole port into one giant slot
    // (review item R5). A populated port reading all-zero is implausible (an
    // assembled rack has divider bits set), so treat it as "not scanned yet"
    // and retry a few times before accepting it.
    for (int p = 0; p < N_PORTS; ++p) {
        if (s_module_count[p] == 0) continue;
        uint32_t words[MAX_MODULES] = { 0 };
        int nmods = 0;
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (rs485::read_inputs((uint8_t)p, words, MAX_MODULES, &nmods) != rs485::Status::Ok)
                break;
            bool any = false;
            for (int m = 0; m < nmods && m < MAX_MODULES; ++m)
                if (words[m]) { any = true; break; }
            if (any) break;          // got a real reading
            delay(40);               // let the Core latch its inputs, then retry
        }
        for (int m = 0; m < nmods && m < MAX_MODULES; ++m) s_inputs[p][m] = words[m];
    }
    s_valid = true;
    return true;
}

} // namespace hw_mirror
