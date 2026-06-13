// =====================================================================
//  InvenTree plugin HTTP client.
//
//  Talks to the SmartReel mock-inventree server (mock-inventree/ in this
//  repo) over HTTP or HTTPS. URL + token come from config_store; an
//  empty URL or token means "not configured" and every call returns
//  Status::NotConfigured without touching the network.
//
//  HTTPS uses NetworkClientSecure with setInsecure() for now -- the mock
//  uses a self-signed cert and pinning is a follow-up. The server's
//  fingerprint is printed by mock-inventree/gen-cert.sh if you want to
//  do the pinning today by hand.
//
//  All public functions here are SYNCHRONOUS and BLOCKING. They must
//  NOT be called from the LVGL task. Callers that live on the LVGL
//  task (UI screens) spawn a one-shot FreeRTOS worker task, run the
//  call there, and post the result back via ui::dispatch_on_lvgl().
//  See net/inv_api_async.h for the small helpers wrapping that pattern.
// =====================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace inv_api {

// ---- Result envelope shared by every call ---------------------------
enum class Status : uint8_t {
    Ok,
    NotConfigured,   // empty URL or token in config
    NoWifi,          // wifi_mgr not in Connected state
    NetworkError,    // socket / TLS / DNS failure
    BadStatus,       // server returned 4xx / 5xx (see http_code + error)
    ParseError,      // 200 OK but body wasn't valid JSON / shape we expect
    Timeout,
};

const char* status_str(Status s);

// ---- Resource shapes mirroring the plugin API -----------------------
struct Part {
    char id[24];     // "R-10K-0805"
    char name[40];   // "RES 10kohm 1% 0805"
    char pkg[10];
    char mfg[16];
};

struct Stock {
    int  id;
    Part part;
    int  qty;
    char batch[16];
    char barcode[32];
    int  location_id;
    int  slot_num;   // 0 when not in a slot
};

enum class ResolveType : uint8_t { Unknown, StockItem, ItemPart, Location };

// ---- Result structs (one per call) -----------------------------------
struct HealthResult {
    Status status;
    int    http_code;
    char   error[96];      // human-readable on failure, empty on Ok
    char   server[40];
    char   version[16];
};

struct ResolveResult {
    Status      status;
    int         http_code;
    char        error[96];
    ResolveType type;
    Stock       stock;     // valid iff type == StockItem
    Part        part;      // valid iff type == ItemPart
    char        message[64];
};

struct SlotMutResult {
    // Generic "we mutated a slot" result. Slot details aren't surfaced
    // up here yet because the HMI doesn't need them on every call --
    // the next /rack snapshot will reflect them.
    Status status;
    int    http_code;
    char   error[96];
    int    stock_id;       // /pick and /clear fill these; else 0
    int    moved_to;
};

// One slot from GET /rack. Slimmed to what the HMI consumes.
struct RackSlot {
    uint16_t slot;          // physical anchor position, 1..N
    bool     occupied;      // stock != null server-side
    int      stock_id;
    int      qty;
    Part     part;
};

// Heap-allocate this (e.g. `new RackResult()`): with MAX_SLOTS entries
// it's far too large for a worker-task stack.
struct RackResult {
    static constexpr int MAX_SLOTS = 256;
    Status   status;
    int      http_code;
    char     error[96];
    int      location_id;
    int      n_slots;
    int      pickjobs_available;
    RackSlot slots[MAX_SLOTS];
};

// GET /pickjobs. Mirrors app::PickJob capacities.
struct PickJobItem {
    char part_id[24];
    char part_name[40];
    int  qty;
    bool picked;
};

struct PickJobInfo {
    char        id[12];          // "BO-0042"
    char        name[64];
    char        requested[24];   // ISO timestamp (UI trims for display)
    char        job_status[10];  // pending | partial | done
    PickJobItem items[16];
    int         n_items;
};

struct PickJobsResult {
    static constexpr int MAX_JOBS = 8;
    Status      status;
    int         http_code;
    char        error[96];
    PickJobInfo jobs[MAX_JOBS];
    int         n_jobs;
};

struct JobPickResult {
    Status status;
    int    http_code;
    char   error[96];
    bool   item_picked;
    char   job_status[10];
};

// ---- Setup ----------------------------------------------------------
// One-time init. Safe to call before WiFi is up; this only sets
// timeouts and disables TLS verification (dev). Re-callable.
void init();

// True when we have a non-empty URL + token in config AND wifi_mgr
// reports Connected. UI code that wants to know "should I even try?"
// reads this.
bool configured();

// Write a fresh op_id into `out`. Format: "hmi-<chip-id>-<millis>-<n>".
// Used as the idempotency key by every mutating call. Buffer should be
// at least 40 bytes; the function refuses to write past `out_size`.
void make_op_id(char* out, size_t out_size);

// ---- Cached "last contact" state for the UI -------------------------
// Updated on every successful health() / resolve_barcode(). UI reads
// these to show "online: server v0.1.0 -- last contacted 14:02" or
// "offline: connection refused".
const HealthResult& last_health();
bool                ever_succeeded();   // any 2xx since boot
uint32_t            last_success_ms();  // millis() of last 2xx, or 0

// ---- Sync calls -----------------------------------------------------
HealthResult  health();
ResolveResult resolve_barcode(const char* code, const char* op_id);
SlotMutResult assign_slot   (int slot_num, int stock_item_id, const char* op_id);
// Whole-reel pick (docs/hmi-plugin-api.md): transfers the slot's
// StockItem to the server-configured staging location. No qty.
SlotMutResult pick_slot     (int slot_num, const char* op_id);
SlotMutResult clear_slot    (int slot_num, const char* reason,const char* op_id);
SlotMutResult report_anomaly(const char* kind, int slot_num,
                             const char* detail, const char* op_id);
// POST /rack/register: ensure slot sub-locations 1..n_slots exist.
SlotMutResult register_rack (int n_slots, const char* op_id);

// Caller heap-allocates `out` (see RackResult comment) and zeroes it.
void get_rack    (RackResult& out);
void get_pickjobs(PickJobsResult& out);
JobPickResult pick_job_item(const char* job_id, int item_idx,
                            int slot_num, const char* op_id);

} // namespace inv_api
