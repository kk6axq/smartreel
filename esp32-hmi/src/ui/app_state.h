// =====================================================================
//  Application state shared across screens.
//
//  Mirrors the State object in DisplaySkeleton/index.html. Same shape,
//  same field names where possible, so a designer iterating on the
//  HTML mockup can find the corresponding fields here.
//
//  4 RS485 chains x 16 slots = 64 logical slots.
// =====================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace app {

// ---- Slot ---------------------------------------------------------
enum class SlotState : uint8_t {
    EMPTY,
    OCCUPIED,
    TARGET,    // lit blue (load-target or pick-target)
    PICKED,    // green - pick already done
    ERROR,
    WARN,
};

struct Part {
    char id[24];     // "R-10K-0805"
    char name[40];   // "RES 10kohm 1% 0805"
    char pkg[10];    // "0805"
    char mfg[16];    // "Yageo"
    bool valid;      // false == empty slot
};

struct Slot {
    int       slot;       // 1..64 (logical)
    int       chain;      // 1..4
    int       position;   // 1..16 within chain
    SlotState state;
    Part      part;
    int       qty;
};

static constexpr int N_CHAINS         = 4;
static constexpr int SLOTS_PER_CHAIN  = 16;
static constexpr int N_SLOTS          = N_CHAINS * SLOTS_PER_CHAIN;

// ---- Pick job -----------------------------------------------------
struct PickItemRequest {
    char part_id[24];
    int  qty;
};

struct PickItem {
    char part_id[24];
    char part_name[40];
    int  qty;
    int  slot_num;       // 0 if not located
    bool picked;
};

struct PickJob {
    char     id[12];           // "BO-0042"
    char     name[64];         // "PCB-Rev-A x 5"
    char     requested[8];     // "14:02"
    PickItem items[16];
    int      n_items;
};

static constexpr int MAX_PICK_JOBS = 4;

// ---- Anomaly ------------------------------------------------------
enum class AnomalyKind : uint8_t { None, Removed, Added, Divider };
enum class AnomalyMood : uint8_t { Error, Warn, Info };

struct AnomalyDetailRow {
    char k[16];
    char v[48];
};

struct Anomaly {
    AnomalyKind      kind = AnomalyKind::None;
    AnomalyMood      mood = AnomalyMood::Error;
    char             title[40];
    char             message[160];
    AnomalyDetailRow detail[6];
    int              n_detail;
};

// ---- Load workflow scratch ---------------------------------------
enum class LoadStep : uint8_t { Scan, Placed };

// ---- Top-level state ---------------------------------------------
struct State {
    // Rack
    Slot rack[N_SLOTS];

    // Pick queue
    PickJob pick_jobs[MAX_PICK_JOBS];
    int     n_pick_jobs;
    int     active_pick_idx;       // -1 when no job in progress

    // Load workflow (active only on the Load screen)
    LoadStep load_step;
    Part     load_part;            // valid when load_step == Placed

    // Anomaly (single slot - mockup shows at most one at a time)
    Anomaly anomaly;
    bool    anomaly_visible;       // modal currently raised

    // Connection / settings (read-only display)
    bool    online;
    char    wifi_ssid[24];
    char    inv_url[40];
    int     inv_location_id;
    char    fw_version[12];
};

// Singleton accessor. On first call: creates the state mutex and
// seeds the in-memory store with mock data so the UI has something
// to render even before state_store::load() runs.
State& state();

// Coarse mutex protecting the State struct against torn reads/writes
// across tasks.
//
// Who needs to take it:
//   - Mutators on any task (today: UI handlers on the LVGL task and
//     the RS485 event dispatcher, which lv_async_call's onto the
//     LVGL task too).
//   - The state_store writer task, briefly, while memcpy'ing a
//     snapshot.
// Who does NOT need to take it:
//   - UI builders / readers on the LVGL task. They're single-writer
//     by convention (all mutations route through the LVGL task), so
//     they can't race with themselves. They only need protection
//     against the writer-task snapshot, which is what the mutator
//     side of the lock provides.
void lock();
void unlock();

// True if state_store::load() actually populated app_state from SD
// at boot; false if we fell back to the mock seed (e.g. no card,
// fresh card, parse error).
bool boot_loaded_from_sd();
void set_boot_loaded_from_sd(bool v);

// Helpers
int  slots_occupied();
int  slots_empty();
int  slots_target();

// Locate a slot by logical number (1..N_SLOTS). Returns nullptr if oob.
Slot*       slot_by_num(int n);
const Slot* slot_at(int chain, int position);  // 1-indexed both args

// Find first occupied slot holding the given part_id.
const Slot* find_part(const char* part_id);

// Pick-job helpers
void resolve_pick_locations(PickJob& j);
int  pick_job_done_count(const PickJob& j);

// Mock-data triggers (called from UI handlers in the prototype build).
void mock_simulate_load_scan();      // pick a random part, light empties
void mock_place_reel(int slot_num);  // commit load to that slot
void mock_cancel_load();
void mock_manual_pick(int slot_num);
void mock_start_pick(int idx);
void mock_raise_anomaly(AnomalyKind k);
void mock_resolve_anomaly();

} // namespace app
