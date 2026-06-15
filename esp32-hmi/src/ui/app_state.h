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

// A logical slot, possibly combining several physical reel-slots (when
// the dividers between them are pulled). `slot` is the logical number
// (the lowest base number in the combined run); numbers can have gaps.
struct Slot {
    int       slot;       // logical number (stable; gaps allowed)
    int       chain;      // = port + 1 (display)
    int       position;   // = module*16 + mslot + 1, 1-based within port (display)
    int       width;      // physical reel-slots spanned (1 = standard width)
    uint8_t   port;       // 0..3
    uint8_t   module;     // module of the first physical slot in the run
    uint8_t   mslot;      // slot-in-module of the first physical slot (0..15)
    SlotState state;
    Part      part;
    int       qty;
};

// Rack geometry. The rack is now DYNAMIC: built at runtime from live (or
// committed) reel topology + divider layout. These are capacities only.
static constexpr int N_PORTS           = 4;
static constexpr int MAX_LOGICAL_SLOTS = N_PORTS * 4 * 16;   // 256 (4 modules x 16/port)
static constexpr int N_SLOTS           = MAX_LOGICAL_SLOTS;  // array capacity

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
    char     status[10];       // server-derived: pending | partial | done
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
    int              slot_num = 0;   // offending logical slot (0 = unknown);
                                     // lets the modal act on the real slot
                                     // (e.g. gated inventory unload, item 3)
};

// ---- Load workflow scratch ---------------------------------------
enum class LoadStep : uint8_t { Scan, Placed };

// ---- Top-level state ---------------------------------------------
struct State {
    // Rack (dynamic): rack[0..n_rack-1] are the live logical slots.
    Slot rack[MAX_LOGICAL_SLOTS];
    int  n_rack;

    // Pick queue
    PickJob pick_jobs[MAX_PICK_JOBS];
    int     n_pick_jobs;
    int     active_pick_idx;       // -1 when no job in progress

    // Load workflow (active only on the Load screen)
    LoadStep load_step;
    Part     load_part;            // valid once a scan is locked in
    int      load_stock_id;        // InvenTree StockItem id from the
                                   // resolve, 0 when offline/unknown
    int      load_qty;             // qty from the resolve, 0 when unknown
    bool     load_scan_locked;     // true == a scan is locked; ignore
                                   // further scans until rescan

    // Pick-out workflow (View screen): logical slot armed to be pulled
    // out of inventory, or -1 when none. Removing the reel from this
    // slot confirms the removal; any other removal is still a tamper.
    int      pick_out_slot;

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

// Locate a slot by logical number. Linear scan (numbers have gaps).
Slot*       slot_by_num(int n);
// The logical slot containing physical (port, module, mslot). nullptr if absent.
const Slot* slot_at_physical(int port, int module, int mslot);

// Rebuild the dynamic rack from the effective topology: the committed
// config if commissioned, else the live hardware mirror. Preserves slot
// contents (part/qty) by logical number across rebuilds and derives
// presence from the hardware mirror. Call after a topology change.
void rebuild_rack();

// Find first occupied slot holding the given part_id.
const Slot* find_part(const char* part_id);

// Pick-job helpers
void resolve_pick_locations(PickJob& j);
int  pick_job_done_count(const PickJob& j);

// ---- Load workflow (real QR scan only) ----------------------------
// Lock the given Part as the loaded part. Called by the Load screen
// after a successful POST /barcode/resolve. No-op when a scan is
// already locked. stock_id/qty come from the resolve; a bare-part
// resolve (no stock item) passes 0 and the placement stays local.
void load_apply_part(const Part& p, int stock_id = 0, int qty = 0);

// Clear the locked scan and go back to watching for a fresh code.
void load_rescan();

// Commit the locked scan: light all empty slots as TARGET and advance
// to the placement step. No-op unless a scan is locked.
void load_begin_placement();

// Remove the reel in a logical slot from inventory: clear its part + qty
// and mark the slot empty, then persist. Local inventory only -- reporting
// the removal to InvenTree (inv_api::clear_slot) is a follow-up, same as
// load placement.
void remove_reel(int slot_num);

// ---- Pick-out workflow (View screen) ------------------------------
// Arm a slot for pick-out (records pick_out_slot; the caller lights the
// LED). cancel clears the marker. finish is called from the hardware
// path when the armed reel is physically pulled: it removes the reel
// from inventory and clears the marker. begin is a no-op for an empty
// or non-inventoried slot.
void begin_pick_out(int slot_num);
void cancel_pick_out();
void finish_pick_out(int slot_num);

// Load/pick mutators (called from UI handlers).
void mock_place_reel(int slot_num);  // commit load to that slot
void mock_cancel_load();
void mock_start_pick(int idx);
void mock_raise_anomaly(AnomalyKind k);
void mock_resolve_anomaly();

// Raise a "reel removed" anomaly tied to a REAL slot (vs mock_raise_anomaly,
// which fabricates example content). Fills accurate slot/part detail and
// anomaly.slot_num so the modal can offer a gated inventory unload for that
// exact slot (review item 3). The caller opens the modal.
void raise_removed_anomaly(int slot_num);

} // namespace app
