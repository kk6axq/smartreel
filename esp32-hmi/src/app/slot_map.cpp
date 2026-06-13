// =====================================================================
//  Logical slot numbering over the reel topology (see slot_map.h).
// =====================================================================
#include "app/slot_map.h"

namespace app {

// ---- Base (physical) numbering ------------------------------------
void SlotMap::rebuild(const uint8_t counts[N_PORTS]) {
    int next = 1;
    for (int p = 0; p < N_PORTS; ++p) {
        uint8_t c = counts[p];
        if (c > MAX_MODULES) c = MAX_MODULES;
        module_count[p] = c;
        port_slots[p]   = (int)c * SLOTS_PER_MODULE;
        if (port_slots[p] > 0) { port_start[p] = next; next += port_slots[p]; }
        else                   { port_start[p] = 0; }
    }
    total_slots = next - 1;
}

bool SlotMap::resolve(int base_num, int& port, int& module, int& slot_in_module) const {
    if (base_num < 1 || base_num > total_slots) return false;
    for (int p = 0; p < N_PORTS; ++p) {
        if (port_slots[p] == 0) continue;
        if (base_num >= port_start[p] && base_num < port_start[p] + port_slots[p]) {
            int off = base_num - port_start[p];
            port = p; module = off / SLOTS_PER_MODULE; slot_in_module = off % SLOTS_PER_MODULE;
            return true;
        }
    }
    return false;
}

int SlotMap::base_of(int port, int module, int slot_in_module) const {
    if (port < 0 || port >= N_PORTS) return 0;
    if (module < 0 || module >= module_count[port]) return 0;
    if (slot_in_module < 0 || slot_in_module >= SLOTS_PER_MODULE) return 0;
    if (port_start[port] == 0) return 0;
    return port_start[port] + module * SLOTS_PER_MODULE + slot_in_module;
}

// ---- DividerLayout ------------------------------------------------
bool DividerLayout::is_pulled(uint8_t port, uint8_t module, uint8_t slot) const {
    for (int i = 0; i < n_pulled; ++i)
        if (pulled[i].port == port && pulled[i].module == module && pulled[i].slot == slot)
            return true;
    return false;
}

void DividerLayout::set_pulled(uint8_t port, uint8_t module, uint8_t slot, bool pull) {
    for (int i = 0; i < n_pulled; ++i) {
        if (pulled[i].port == port && pulled[i].module == module && pulled[i].slot == slot) {
            if (!pull) { pulled[i] = pulled[--n_pulled]; }   // remove
            return;
        }
    }
    if (pull && n_pulled < MAX_PULLED) pulled[n_pulled++] = { port, module, slot };
}

// ---- LogicalRack --------------------------------------------------
// True if the divider to the LEFT of (port,module,slot) is a real combine
// boundary that has been pulled (so this slot merges with the run to its
// left). A divider key (p,m,s) is the divider to the RIGHT of slot s (bit
// D_s); the one to the LEFT of slot s is therefore key (p,m,s-1), and at a
// module boundary the divider left of slot 0 is the previous module's
// last-slot divider (p,m-1,15). The very first slot of the port (module 0,
// slot 0) has no divider to its left and is never a combine boundary.
static bool merges_left(const DividerLayout& div, int p, int m, int s) {
    if (s == 0 && m == 0) return false;                                  // port edge
    if (s > 0)            return div.is_pulled((uint8_t)p, (uint8_t)m, (uint8_t)(s - 1));
    return div.is_pulled((uint8_t)p, (uint8_t)(m - 1), 15);              // cross-module
}

void LogicalRack::rebuild(const uint8_t module_count[SlotMap::N_PORTS],
                          const DividerLayout& div) {
    base.rebuild(module_count);
    n_slots = 0;
    int cur = -1;
    int cur_port = -1;
    for (int p = 0; p < SlotMap::N_PORTS; ++p) {
        for (int m = 0; m < base.module_count[p]; ++m) {
            for (int s = 0; s < SlotMap::SLOTS_PER_MODULE; ++s) {
                const int num = base.base_of(p, m, s);
                const bool can_merge = (p == cur_port) && (cur >= 0);
                if (can_merge && merges_left(div, p, m, s)) {
                    slots[cur].width++;
                } else if (n_slots < SlotMap::MAX_SLOTS) {
                    cur = n_slots++;
                    slots[cur] = { num, 1, (uint8_t)p, (uint8_t)m, (uint8_t)s };
                    cur_port = p;
                }
            }
        }
    }
}

const LogicalSlot* LogicalRack::by_num(int num) const {
    for (int i = 0; i < n_slots; ++i) if (slots[i].num == num) return &slots[i];
    return nullptr;
}

const LogicalSlot* LogicalRack::containing(uint8_t port, uint8_t module, uint8_t slot) const {
    const int pix = module * SlotMap::SLOTS_PER_MODULE + slot;
    for (int i = 0; i < n_slots; ++i) {
        if (slots[i].port != port) continue;
        const int start = slots[i].module * SlotMap::SLOTS_PER_MODULE + slots[i].slot;
        if (pix >= start && pix < start + slots[i].width) return &slots[i];
    }
    return nullptr;
}

bool LogicalRack::pixel_span(int num, uint8_t& port, int& start_pixel, int& width) const {
    const LogicalSlot* ls = by_num(num);
    if (!ls) return false;
    port        = ls->port;
    start_pixel = ls->module * SlotMap::SLOTS_PER_MODULE + ls->slot;
    width       = ls->width;
    return true;
}

// ---- Validation ---------------------------------------------------
void validate_topology(const uint8_t committed_counts[SlotMap::N_PORTS],
                       const DividerLayout& committed_div,
                       const uint8_t live_counts[SlotMap::N_PORTS],
                       const uint32_t live_inputs[SlotMap::N_PORTS][SlotMap::MAX_MODULES],
                       Deviation* out, int max, int* n_out) {
    int n = 0;
    auto push = [&](Deviation d) { if (n < max) out[n++] = d; };

    for (int p = 0; p < SlotMap::N_PORTS; ++p) {
        if (committed_counts[p] != live_counts[p]) {
            push({ DeviationKind::ModuleCount, (uint8_t)p, 0, 0,
                   committed_counts[p], live_counts[p] });
            continue;   // counts differ -> per-divider checks on this port are moot
        }
        // Per combine-boundary: committed expects present (not pulled) or
        // absent (pulled). Compare to the live divider bit.
        for (int m = 0; m < committed_counts[p]; ++m) {
            for (int s = 0; s < SlotMap::SLOTS_PER_MODULE; ++s) {
                // D_s is the divider to the RIGHT of slot s; the one past
                // the last slot of the last module is the port edge.
                if (m == committed_counts[p] - 1 && s == SlotMap::SLOTS_PER_MODULE - 1) continue;
                const bool expect_present = !committed_div.is_pulled((uint8_t)p, (uint8_t)m, (uint8_t)s);
                const bool live_present   = divider_present_bit(live_inputs[p][m], s);
                if (expect_present && !live_present)
                    push({ DeviationKind::DividerMissing, (uint8_t)p, (uint8_t)m, (uint8_t)s, 1, 0 });
                else if (!expect_present && live_present)
                    push({ DeviationKind::DividerExtra, (uint8_t)p, (uint8_t)m, (uint8_t)s, 0, 1 });
            }
        }
    }
    if (n_out) *n_out = n;
}

} // namespace app
