#include "ui/app_state.h"
#include "storage/parts_catalog.h"

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

// ---- Parts catalog (mirrors DUMMY_PARTS in the HTML) --------------
struct PartTemplate {
    const char* id;
    const char* name;
    const char* pkg;
    const char* mfg;
};
static const PartTemplate kCatalog[] = {
    {"R-10K-0805",   "RES 10kohm 1% 0805",       "0805",   "Yageo"},
    {"R-1K-0603",    "RES 1kohm 1% 0603",        "0603",   "Yageo"},
    {"R-100R-0603",  "RES 100ohm 1% 0603",       "0603",   "Yageo"},
    {"R-4K7-0805",   "RES 4.7kohm 1% 0805",      "0805",   "Yageo"},
    {"C-100N-0603",  "CAP 100nF X7R 0603",       "0603",   "Murata"},
    {"C-10U-0805",   "CAP 10uF X5R 0805",        "0805",   "Samsung"},
    {"C-1U-0603",    "CAP 1uF X7R 0603",         "0603",   "Murata"},
    {"L-10U-0805",   "IND 10uH 0805",            "0805",   "Coilcraft"},
    {"D-LED-G-0603", "LED green 0603",           "0603",   "Lite-On"},
    {"D-LED-R-0603", "LED red 0603",             "0603",   "Lite-On"},
    {"D-1N4148",     "DIO 1N4148 SOD-123",       "SOD123", "NXP"},
    {"Q-2N7002",     "MOS 2N7002 SOT-23",        "SOT23",  "ON Semi"},
    {"IC-LM358",     "OPA LM358 SOIC-8",         "SOIC8",  "TI"},
    {"IC-555",       "TIM NE555 SOIC-8",         "SOIC8",  "TI"},
    {"IC-ESP32S3",   "MCU ESP32-S3-WROOM",       "Module", "Espressif"},
    {"IC-RP2040",    "MCU RP2040 QFN-56",        "QFN56",  "RaspPi"},
    {"CONN-USBC",    "CONN USB-C receptacle",    "SMD",    "Molex"},
    {"CONN-JST",     "CONN JST-PH 2-pin",        "TH",     "JST"},
};
static constexpr int kCatalogN = sizeof(kCatalog) / sizeof(kCatalog[0]);

static void copy_part(Part& dst, const PartTemplate& src) {
    snprintf(dst.id,   sizeof(dst.id),   "%s", src.id);
    snprintf(dst.name, sizeof(dst.name), "%s", src.name);
    snprintf(dst.pkg,  sizeof(dst.pkg),  "%s", src.pkg);
    snprintf(dst.mfg,  sizeof(dst.mfg),  "%s", src.mfg);
    dst.valid = true;
}

// Deterministic PRNG so we get the same mock layout across boots --
// makes screenshots reproducible.
static uint32_t s_rng = 0xCAFE1234;
static uint32_t rng() {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

// ---- Build initial rack -------------------------------------------
static void build_rack(State& st) {
    int idx = 0;
    int slot_num = 1;
    for (int chain = 1; chain <= N_CHAINS; ++chain) {
        for (int pos = 1; pos <= SLOTS_PER_CHAIN; ++pos, ++idx, ++slot_num) {
            Slot& s = st.rack[idx];
            s.slot     = slot_num;
            s.chain    = chain;
            s.position = pos;

            // ~60% occupancy
            bool occupied = (rng() % 100) < 60;
            if (occupied) {
                s.state = SlotState::OCCUPIED;
                copy_part(s.part, kCatalog[rng() % kCatalogN]);
                s.qty = 50 + (int)(rng() % 1450);
            } else {
                s.state = SlotState::EMPTY;
                s.part.valid = false;
                s.qty = 0;
            }
        }
    }
}

// ---- Build pick jobs ----------------------------------------------
static void add_pick_item(PickJob& j, const char* part_id, int qty) {
    if (j.n_items >= (int)(sizeof(j.items) / sizeof(j.items[0]))) return;
    PickItem& it = j.items[j.n_items++];
    snprintf(it.part_id, sizeof(it.part_id), "%s", part_id);
    it.part_name[0] = 0;
    it.qty       = qty;
    it.slot_num  = 0;
    it.picked    = false;
}

static void build_pick_jobs(State& st) {
    st.n_pick_jobs = 0;

    auto& a = st.pick_jobs[st.n_pick_jobs++];
    snprintf(a.id,        sizeof(a.id),        "BO-0042");
    snprintf(a.name,      sizeof(a.name),      "PCB-Rev-A x 5");
    snprintf(a.requested, sizeof(a.requested), "14:02");
    a.n_items = 0;
    add_pick_item(a, "R-10K-0805",    50);
    add_pick_item(a, "C-100N-0603",   20);
    add_pick_item(a, "IC-LM358",       5);
    add_pick_item(a, "D-LED-G-0603",   5);

    auto& b = st.pick_jobs[st.n_pick_jobs++];
    snprintf(b.id,        sizeof(b.id),        "BO-0043");
    snprintf(b.name,      sizeof(b.name),      "Test-Build-12 proto");
    snprintf(b.requested, sizeof(b.requested), "14:18");
    b.n_items = 0;
    add_pick_item(b, "IC-ESP32S3",  3);
    add_pick_item(b, "CONN-USBC",   3);
    add_pick_item(b, "C-10U-0805", 12);

    auto& c = st.pick_jobs[st.n_pick_jobs++];
    snprintf(c.id,        sizeof(c.id),        "BO-0044");
    snprintf(c.name,      sizeof(c.name),      "Sensor-Hub v2");
    snprintf(c.requested, sizeof(c.requested), "14:32");
    c.n_items = 0;
    add_pick_item(c, "IC-RP2040",   2);
    add_pick_item(c, "R-4K7-0805", 24);
    add_pick_item(c, "Q-2N7002",    8);
    add_pick_item(c, "D-1N4148",   16);
    add_pick_item(c, "CONN-JST",    4);
}

static void seed(State& st) {
    memset(&st, 0, sizeof(st));
    st.online = true;
    st.active_pick_idx = -1;
    st.load_step = LoadStep::Scan;
    snprintf(st.wifi_ssid, sizeof(st.wifi_ssid), "labnet-2g");
    snprintf(st.inv_url,   sizeof(st.inv_url),   "https://inv.lab.local");
    st.inv_location_id = 42;
    snprintf(st.fw_version, sizeof(st.fw_version), "v0.4.2");
    build_rack(st);
    build_pick_jobs(st);
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
int slots_occupied() {
    int n = 0;
    for (auto& s : state().rack) if (s.state == SlotState::OCCUPIED) n++;
    return n;
}
int slots_empty() {
    int n = 0;
    for (auto& s : state().rack) if (s.state == SlotState::EMPTY) n++;
    return n;
}
int slots_target() {
    int n = 0;
    for (auto& s : state().rack) if (s.state == SlotState::TARGET) n++;
    return n;
}

Slot* slot_by_num(int n) {
    if (n < 1 || n > N_SLOTS) return nullptr;
    return &state().rack[n - 1];
}

const Slot* slot_at(int chain, int position) {
    if (chain < 1 || chain > N_CHAINS) return nullptr;
    if (position < 1 || position > SLOTS_PER_CHAIN) return nullptr;
    int idx = (chain - 1) * SLOTS_PER_CHAIN + (position - 1);
    return &state().rack[idx];
}

const Slot* find_part(const char* part_id) {
    for (auto& s : state().rack) {
        if (s.state != SlotState::OCCUPIED) continue;
        if (!s.part.valid) continue;
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

// ---- Mock-data triggers (the prototype build wires UI buttons here) ---
//
// These now also push dirty-bits at state_store so persistent changes
// land on the SD card. The forward-declaration of the state_store
// dirty API at the top of this file avoids a circular include.
// ---- Load workflow (real QR scan + mock placement) ----------------
bool load_apply_scan(const char* qr) {
    State& st = state();
    if (st.load_scan_locked) return false;     // already locked; ignore
    Part p;
    if (!parts_catalog::lookup(qr, p)) return false;
    lock();
    st.load_part        = p;
    st.load_scan_locked = true;
    unlock();
    return true;
}

void load_apply_part(const Part& p) {
    State& st = state();
    if (st.load_scan_locked) return;           // already locked; ignore
    lock();
    st.load_part        = p;
    st.load_scan_locked = true;
    unlock();
}

void load_rescan() {
    State& st = state();
    lock();
    st.load_scan_locked = false;
    st.load_part.valid  = false;
    unlock();
}

void load_begin_placement() {
    State& st = state();
    if (!st.load_scan_locked) return;
    lock();
    for (auto& s : st.rack) {
        if (s.state == SlotState::EMPTY) s.state = SlotState::TARGET;
    }
    st.load_step = LoadStep::Placed;
    unlock();
    // TARGET collapses to EMPTY on disk, so this is workflow-only;
    // no dirty mark.
}

void mock_simulate_load_scan() {
    State& st = state();
    if (st.load_scan_locked) return;
    // Prefer a real catalog QR so the simulated path is identical to a
    // live scan; fall back to the built-in catalog if no SD parts.json.
    int n = parts_catalog::count();
    if (n > 0 && load_apply_scan(parts_catalog::qr_at((int)(rng() % n)))) return;
    lock();
    copy_part(st.load_part, kCatalog[rng() % kCatalogN]);
    st.load_scan_locked = true;
    unlock();
}

void mock_place_reel(int slot_num) {
    State& st = state();
    Slot* chosen = slot_by_num(slot_num);
    if (!chosen) return;
    lock();
    // Clear other lit
    for (auto& s : st.rack) {
        if (s.state == SlotState::TARGET && s.slot != slot_num)
            s.state = SlotState::EMPTY;
    }
    chosen->state = SlotState::OCCUPIED;
    chosen->part  = st.load_part;
    chosen->qty   = 100 + (int)(rng() % 900);
    st.load_step = LoadStep::Scan;
    st.load_part.valid = false;
    st.load_scan_locked = false;
    unlock();
    state_store::mark_slot_dirty(slot_num);
}

void mock_cancel_load() {
    State& st = state();
    lock();
    for (auto& s : st.rack) {
        if (s.state == SlotState::TARGET) s.state = SlotState::EMPTY;
    }
    st.load_step = LoadStep::Scan;
    st.load_part.valid = false;
    st.load_scan_locked = false;
    unlock();
}

void mock_manual_pick(int slot_num) {
    Slot* s = slot_by_num(slot_num);
    if (!s) return;
    lock();
    s->state = SlotState::EMPTY;
    s->part.valid = false;
    s->qty = 0;
    unlock();
    state_store::mark_slot_dirty(slot_num);
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
            for (auto& slot : st.rack) {
                if (slot.state == SlotState::OCCUPIED) { s = &slot; break; }
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
            for (auto& slot : st.rack) {
                if (slot.state == SlotState::EMPTY) { s = &slot; break; }
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
                        "A divider button changed while not in Load or Divider Maintenance mode.");
            anomaly_kv(a, "Position", "between #28 and #29");
            anomaly_kv(a, "Change",   "divider removed");
            anomaly_kv(a, "Action",   "Replace divider, or enter Maintenance");
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

} // namespace app
