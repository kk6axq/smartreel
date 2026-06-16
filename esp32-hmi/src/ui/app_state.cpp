#include "ui/app_state.h"
#include "storage/config_store.h"
#include "app/slot_map.h"
#include "app/hw_mirror.h"
#include "app/inv_sync.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Forward-declare state_store's dirty-marking API so we can call it
// without including state_store.h here (which would create a cycle:
// state_store.h includes app_state.h).
namespace state_store {
    void mark_slot_dirty(int slot_num);
    void mark_all_slots_dirty();
    void mark_jobs_dirty();
    void log_anomaly(const app::Anomaly& a);
}

namespace app {

static State              g_state;
static bool               g_inited        = false;
static SemaphoreHandle_t  g_mutex         = nullptr;
static bool               g_loaded_from_sd = false;

// Deterministic PRNG (qty fallback for a bare-part load with no
// server-reported quantity).
static uint32_t s_rng = 0xCAFE1234;
static uint32_t rng() {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

// ---- Build the dynamic rack from topology -------------------------
// The effective topology is the committed config once commissioned,
// otherwise the live hardware mirror (with dividers derived from the
// live divider bits so the rack reflects what's physically installed).
static void effective_topology(uint8_t counts[N_PORTS], DividerLayout& div) {
    const auto& rc = config_store::cfg().rack;
    div.clear();
    if (rc.committed) {
        for (int p = 0; p < N_PORTS; ++p) counts[p] = rc.module_count[p];
        div = rc.dividers;
    } else {
        for (int p = 0; p < N_PORTS; ++p) counts[p] = hw_mirror::module_count(p);
        // Derive pulled dividers from the live bits (absent divider = pulled).
        for (int p = 0; p < N_PORTS; ++p)
            for (int m = 0; m < counts[p]; ++m)
                for (int s = 0; s < SlotMap::SLOTS_PER_MODULE; ++s) {
                    // D_s is the divider to the RIGHT of slot s; the one
                    // past the last slot of the last module is the port edge.
                    if (m == counts[p] - 1 && s == SlotMap::SLOTS_PER_MODULE - 1) continue;
                    if (!hw_mirror::divider_present(p, m, s))
                        div.set_pulled((uint8_t)p, (uint8_t)m, (uint8_t)s, true);
                }
    }
}

// True if any physical reel-slot in a logical run reads present.
static bool run_present(const LogicalSlot& ls) {
    for (int w = 0; w < ls.width; ++w) {
        int pix = ls.module * SlotMap::SLOTS_PER_MODULE + ls.slot + w;
        if (hw_mirror::slot_present(ls.port, pix / SlotMap::SLOTS_PER_MODULE,
                                            pix % SlotMap::SLOTS_PER_MODULE))
            return true;
    }
    return false;
}

void rebuild_rack() {
    State& st = state();

    // Snapshot existing contents so they survive the rebuild. We key by
    // logical number first, but ALSO remember the physical anchor
    // (port,module,mslot): if a re-number changes slot numbers (e.g. a boot
    // divider misread corrects itself), contents follow the physical position
    // rather than being dropped (review item R6).
    struct Saved { int num; uint8_t port, module, mslot; SlotState state; Part part; int qty; };
    static Saved   saved[MAX_LOGICAL_SLOTS];
    static LogicalRack lr;             // large-ish; keep off the stack
    int nsaved = 0;
    for (int i = 0; i < st.n_rack; ++i)
        saved[nsaved++] = { st.rack[i].slot, st.rack[i].port, st.rack[i].module,
                            st.rack[i].mslot, st.rack[i].state, st.rack[i].part,
                            st.rack[i].qty };

    uint8_t counts[N_PORTS];
    DividerLayout div;
    effective_topology(counts, div);
    lr.rebuild(counts, div);

    lock();
    st.n_rack = lr.n_slots;
    for (int i = 0; i < lr.n_slots; ++i) {
        const LogicalSlot& ls = lr.slots[i];
        Slot& s = st.rack[i];
        s.slot     = ls.num;
        s.width    = ls.width;
        s.port     = ls.port;
        s.module   = ls.module;
        s.mslot    = ls.slot;
        s.chain    = ls.port + 1;
        s.position = ls.module * SlotMap::SLOTS_PER_MODULE + ls.slot + 1;

        const bool present = run_present(ls);
        const Saved* sv = nullptr;
        for (int k = 0; k < nsaved; ++k) if (saved[k].num == ls.num) { sv = &saved[k]; break; }
        // No same-number match (the rack re-numbered): fall back to the slot
        // anchored at the same physical (port,module,mslot) so a placed reel
        // isn't lost (review item R6).
        if (!sv)
            for (int k = 0; k < nsaved; ++k)
                if (saved[k].port == ls.port && saved[k].module == ls.module
                    && saved[k].mslot == ls.slot && saved[k].part.valid) {
                    sv = &saved[k]; break;
                }
        if (sv) {
            s.state = sv->state;
            s.part  = sv->part;
            s.qty   = sv->qty;
            // Refresh the presence-derived base state (leave workflow
            // overrides TARGET/PICKED/WARN/ERROR untouched).
            if (s.state == SlotState::OCCUPIED || s.state == SlotState::EMPTY)
                s.state = present ? SlotState::OCCUPIED : SlotState::EMPTY;
        } else {
            s.state      = present ? SlotState::OCCUPIED : SlotState::EMPTY;
            s.part.valid = false;
            s.qty        = 0;
        }
    }
    unlock();
}

static void seed(State& st) {
    memset(&st, 0, sizeof(st));
    st.online = true;
    st.active_pick_idx = -1;
    st.pick_out_slot = -1;
    st.load_step = LoadStep::Scan;
    snprintf(st.wifi_ssid, sizeof(st.wifi_ssid), "labnet-2g");
    snprintf(st.inv_url,   sizeof(st.inv_url),   "https://inv.lab.local");
    st.inv_location_id = 42;
    snprintf(st.fw_version, sizeof(st.fw_version), "v0.4.2");
    st.n_rack = 0;            // populated by rebuild_rack() once topology is known
    // Pick jobs come live from InvenTree (GET /pickjobs via inv_sync);
    // start empty rather than seeding fake placeholders.
    st.n_pick_jobs = 0;
}

State& state() {
    if (!g_inited) {
        // First call comes from main.cpp setup before any tasks
        // exist, so this is safe without external synchronisation.
        g_mutex = xSemaphoreCreateMutex();
        seed(g_state);
        g_inited = true;
    }
    return g_state;
}

void lock()   { if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY); }
void unlock() { if (g_mutex) xSemaphoreGive(g_mutex); }

bool boot_loaded_from_sd()           { return g_loaded_from_sd; }
void set_boot_loaded_from_sd(bool v) { g_loaded_from_sd = v; }

// ---- Helpers ------------------------------------------------------
static int count_state(SlotState want) {
    State& st = state();
    int n = 0;
    for (int i = 0; i < st.n_rack; ++i) if (st.rack[i].state == want) n++;
    return n;
}
int slots_occupied() { return count_state(SlotState::OCCUPIED); }
int slots_empty()    { return count_state(SlotState::EMPTY); }
int slots_target()   { return count_state(SlotState::TARGET); }

Slot* slot_by_num(int n) {
    State& st = state();
    for (int i = 0; i < st.n_rack; ++i) if (st.rack[i].slot == n) return &st.rack[i];
    return nullptr;
}

const Slot* slot_at_physical(int port, int module, int mslot) {
    State& st = state();
    const int pix = module * 16 + mslot;
    for (int i = 0; i < st.n_rack; ++i) {
        const Slot& s = st.rack[i];
        if (s.port != port) continue;
        const int start = s.module * 16 + s.mslot;
        if (pix >= start && pix < start + s.width) return &s;
    }
    return nullptr;
}

const Slot* find_part(const char* part_id) {
    State& st = state();
    for (int i = 0; i < st.n_rack; ++i) {
        const Slot& s = st.rack[i];
        if (s.state != SlotState::OCCUPIED || !s.part.valid) continue;
        if (strcmp(s.part.id, part_id) == 0) return &s;
    }
    return nullptr;
}

// ---- Pick helpers --------------------------------------------------
void resolve_pick_locations(PickJob& j) {
    for (int i = 0; i < j.n_items; ++i) {
        const Slot* s = find_part(j.items[i].part_id);
        if (s) {
            j.items[i].slot_num = s->slot;
            snprintf(j.items[i].part_name, sizeof(j.items[i].part_name),
                     "%s", s->part.name);
        } else {
            j.items[i].slot_num = 0;
            snprintf(j.items[i].part_name, sizeof(j.items[i].part_name),
                     "%s", j.items[i].part_id);
        }
    }
}

int pick_job_done_count(const PickJob& j) {
    int n = 0;
    for (int i = 0; i < j.n_items; ++i) if (j.items[i].picked) n++;
    return n;
}

// ---- Load workflow (real QR scan) ---------------------------------
//
// Mutators push dirty-bits at state_store so persistent changes land on
// the SD card. The forward-declaration of the state_store dirty API at
// the top of this file avoids a circular include.
void load_apply_part(const Part& p, int stock_id, int qty) {
    State& st = state();
    if (st.load_scan_locked) return;           // already locked; ignore
    lock();
    st.load_part        = p;
    st.load_stock_id    = stock_id;
    st.load_qty         = qty;
    st.load_scan_locked = true;
    unlock();
}

void load_rescan() {
    State& st = state();
    lock();
    st.load_scan_locked = false;
    st.load_part.valid  = false;
    st.load_stock_id    = 0;
    st.load_qty         = 0;
    unlock();
}

void load_begin_placement() {
    State& st = state();
    if (!st.load_scan_locked) return;
    lock();
    for (int i = 0; i < st.n_rack; ++i) {
        if (st.rack[i].state == SlotState::EMPTY) st.rack[i].state = SlotState::TARGET;
    }
    st.load_step = LoadStep::Placed;
    unlock();
    // TARGET collapses to EMPTY on disk, so this is workflow-only;
    // no dirty mark.
}

void mock_place_reel(int slot_num) {
    State& st = state();
    Slot* chosen = slot_by_num(slot_num);
    if (!chosen) return;
    const int stock_id = st.load_stock_id;
    lock();
    // Clear other lit
    for (int i = 0; i < st.n_rack; ++i) {
        if (st.rack[i].state == SlotState::TARGET && st.rack[i].slot != slot_num)
            st.rack[i].state = SlotState::EMPTY;
    }
    chosen->state = SlotState::OCCUPIED;
    chosen->part  = st.load_part;
    // Real qty from the InvenTree resolve; the random fallback only
    // covers a bare-part resolve that reported no quantity.
    chosen->qty   = st.load_qty > 0 ? st.load_qty : 100 + (int)(rng() % 900);
    st.load_step = LoadStep::Scan;
    st.load_part.valid = false;
    st.load_stock_id = 0;
    st.load_qty = 0;
    st.load_scan_locked = false;
    unlock();
    state_store::mark_slot_dirty(slot_num);
    // Report the placement to InvenTree (user story 1: the StockItem
    // moves into the slot's sub-location). Offline-catalog loads have
    // no stock id and stay local.
    if (stock_id > 0) inv_sync::queue_assign(slot_num, stock_id);
}

void mock_cancel_load() {
    State& st = state();
    lock();
    for (int i = 0; i < st.n_rack; ++i) {
        if (st.rack[i].state == SlotState::TARGET) st.rack[i].state = SlotState::EMPTY;
    }
    st.load_step = LoadStep::Scan;
    st.load_part.valid = false;
    st.load_stock_id = 0;
    st.load_qty = 0;
    st.load_scan_locked = false;
    unlock();
}

void remove_reel(int slot_num) {
    Slot* s = slot_by_num(slot_num);
    if (!s) return;
    lock();
    s->state = SlotState::EMPTY;
    s->part.valid = false;
    s->qty = 0;
    unlock();
    state_store::mark_slot_dirty(slot_num);
}

// ---- Pick-out workflow --------------------------------------------
// pick_out_slot is touched only on the LVGL task (UI handlers + the
// RS485 dispatcher both run there), so a plain int write is race-free.
void begin_pick_out(int slot_num) {
    Slot* s = slot_by_num(slot_num);
    if (!s || !s->part.valid) return;       // only inventoried reels
    state().pick_out_slot = slot_num;
}

void cancel_pick_out() {
    state().pick_out_slot = -1;
}

void finish_pick_out(int slot_num) {
    remove_reel(slot_num);
    state().pick_out_slot = -1;
    // User story 3: the reel's StockItem transfers to the Staging
    // location once the user physically takes it. Arming is blocked
    // while offline, so by the time we get here the server op is
    // expected to succeed (and op_id retries cover blips).
    inv_sync::queue_pick(slot_num);
}

void mock_start_pick(int idx) {
    State& st = state();
    if (idx < 0 || idx >= st.n_pick_jobs) return;
    lock();
    st.active_pick_idx = idx;
    PickJob& j = st.pick_jobs[idx];
    resolve_pick_locations(j);
    for (int i = 0; i < j.n_items; ++i) {
        Slot* s = slot_by_num(j.items[i].slot_num);
        if (s) s->state = SlotState::TARGET;
    }
    unlock();
    state_store::mark_jobs_dirty();
}

// ---- Anomaly mock --------------------------------------------------
static void anomaly_set(Anomaly& a, AnomalyKind k, AnomalyMood m,
                        const char* title, const char* msg) {
    a.kind = k;
    a.mood = m;
    snprintf(a.title,   sizeof(a.title),   "%s", title);
    snprintf(a.message, sizeof(a.message), "%s", msg);
    a.n_detail = 0;
}
static void anomaly_kv(Anomaly& a, const char* k, const char* v) {
    if (a.n_detail >= (int)(sizeof(a.detail) / sizeof(a.detail[0]))) return;
    AnomalyDetailRow& r = a.detail[a.n_detail++];
    snprintf(r.k, sizeof(r.k), "%s", k);
    snprintf(r.v, sizeof(r.v), "%s", v);
}

void mock_raise_anomaly(AnomalyKind k) {
    State& st = state();
    lock();
    Anomaly& a = st.anomaly;

    char buf[64];
    switch (k) {
        case AnomalyKind::Removed: {
            anomaly_set(a, k, AnomalyMood::Error,
                        "Reel removed",
                        "A reel was taken from a slot that is not part of an active pick job.");
            const Slot* s = nullptr;
            for (int i = 0; i < st.n_rack; ++i) {
                if (st.rack[i].state == SlotState::OCCUPIED) { s = &st.rack[i]; break; }
            }
            if (s) {
                snprintf(buf, sizeof(buf), "#%d (chain %d)", s->slot, s->chain);
                anomaly_kv(a, "Slot", buf);
                anomaly_kv(a, "Part", s->part.valid ? s->part.name : "unknown");
            }
            anomaly_kv(a, "Last seen", "2 sec ago");
            anomaly_kv(a, "Action",    "Replace the reel, or confirm removal");
            break;
        }
        case AnomalyKind::Added: {
            anomaly_set(a, k, AnomalyMood::Warn,
                        "Unexpected reel placement",
                        "A reel was placed in a slot without a matching scan.");
            const Slot* s = nullptr;
            for (int i = 0; i < st.n_rack; ++i) {
                if (st.rack[i].state == SlotState::EMPTY) { s = &st.rack[i]; break; }
            }
            if (s) {
                snprintf(buf, sizeof(buf), "#%d (chain %d)", s->slot, s->chain);
                anomaly_kv(a, "Slot", buf);
            }
            anomaly_kv(a, "Detected", "reel-presence button asserted");
            anomaly_kv(a, "Action",   "Scan the reel's QR code, or remove it");
            break;
        }
        case AnomalyKind::Divider: {
            anomaly_set(a, k, AnomalyMood::Warn,
                        "Divider state changed",
                        "A divider changed. If a reel is loaded across this slot, "
                        "unload it before changing the divider. Otherwise restore "
                        "the divider or enter Divider Maintenance.");
            anomaly_kv(a, "Action",   "Restore, or unload first");
            break;
        }
        default:
            unlock();
            return;
    }
    st.anomaly_visible = true;
    unlock();
    state_store::log_anomaly(a);
}

void mock_resolve_anomaly() {
    State& st = state();
    lock();
    st.anomaly.kind = AnomalyKind::None;
    st.anomaly_visible = false;
    unlock();
}

void raise_removed_anomaly(int slot_num) {
    State& st = state();
    lock();
    Anomaly& a = st.anomaly;
    anomaly_set(a, AnomalyKind::Removed, AnomalyMood::Error,
                "Reel removed",
                "A reel was pulled from a slot with no active pick job. "
                "Replace it, or unload it from inventory.");
    a.slot_num = slot_num;
    const Slot* s = slot_by_num(slot_num);
    char buf[64];
    if (s) {
        snprintf(buf, sizeof(buf), "#%d (chain %d)", s->slot, s->chain);
        anomaly_kv(a, "Slot", buf);
        anomaly_kv(a, "Part", s->part.valid ? s->part.name : "unknown");
    }
    anomaly_kv(a, "Action", "Replace or unload");
    st.anomaly_visible = true;
    unlock();
    state_store::log_anomaly(a);
}

} // namespace app
