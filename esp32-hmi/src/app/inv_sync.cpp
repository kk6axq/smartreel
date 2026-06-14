#include "app/inv_sync.h"

#include "net/inv_api.h"
#include "net/wifi_mgr.h"
#include "app/hw_mirror.h"
#include "app/slot_map.h"
#include "app/leds.h"
#include "ui/app_state.h"
#include "ui/anomaly_modal.h"
#include "ui/screen_manager.h"
#include "util/lvgl_async.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp32-hal-log.h>
#include <string.h>
#include <stdio.h>

// state_store dirty-marking (forward decls; see app_state.cpp for why
// we don't include state_store.h here).
namespace state_store {
    void mark_slot_dirty(int slot_num);
    void mark_jobs_dirty();
}

// Action log to the USB serial console, always on (independent of
// CORE_DEBUG_LEVEL) so it can be captured and pasted back during bench
// bring-up. Greppable "[inv]" prefix; one line per meaningful action.
#define SR_LOG(fmt, ...) Serial.printf("[inv] " fmt "\n", ##__VA_ARGS__)

namespace inv_sync {

namespace {

// ---- Intervals ------------------------------------------------------
constexpr uint32_t TICK_MS            = 500;
constexpr uint32_t HEALTH_OK_IVL_MS   = 15000;   // re-check while online
constexpr uint32_t HEALTH_BAD_IVL_MS  = 5000;    // retry faster while down
constexpr uint32_t RACK_IVL_MS        = 60000;   // periodic reconcile
constexpr uint32_t ONLINE_WINDOW_MS   = 45000;   // health age that still counts as online
constexpr int      OP_MAX_ATTEMPTS    = 20;      // transient-failure retries per op

// ---- Mutation queue --------------------------------------------------
enum class OpType : uint8_t { Assign, Pick, JobPick, Clear, Anomaly };

struct Op {
    OpType  type;
    int     slot;
    int     stock_id;       // Assign
    int     item_idx;       // JobPick
    char    job_id[12];     // JobPick
    char    text[96];       // Clear: reason / Anomaly: detail
    char    kind[10];       // Anomaly
    char    op_id[40];
    uint8_t attempts;
};

constexpr int QUEUE_CAP = 16;
Op  g_queue[QUEUE_CAP];
int g_q_head = 0, g_q_count = 0;
SemaphoreHandle_t g_q_mutex = nullptr;

// ---- Worker flags -----------------------------------------------------
volatile bool g_want_jobs = false;
volatile bool g_want_rack = false;
volatile bool g_registered = false;

// Online tracking (written by worker, read by UI -- single word).
volatile bool     g_online = false;
volatile uint32_t g_last_health_ok = 0;

bool q_push(const Op& op) {
    bool ok = false;
    xSemaphoreTake(g_q_mutex, portMAX_DELAY);
    if (g_q_count < QUEUE_CAP) {
        g_queue[(g_q_head + g_q_count) % QUEUE_CAP] = op;
        g_q_count++;
        ok = true;
    }
    xSemaphoreGive(g_q_mutex);
    if (!ok) {
        log_e("inv_sync: op queue full, dropping %d (slot %d)", (int)op.type, op.slot);
    }
    return ok;
}

bool q_peek(Op& out) {
    bool ok = false;
    xSemaphoreTake(g_q_mutex, portMAX_DELAY);
    if (g_q_count > 0) { out = g_queue[g_q_head]; ok = true; }
    xSemaphoreGive(g_q_mutex);
    return ok;
}

void q_pop() {
    xSemaphoreTake(g_q_mutex, portMAX_DELAY);
    if (g_q_count > 0) { g_q_head = (g_q_head + 1) % QUEUE_CAP; g_q_count--; }
    xSemaphoreGive(g_q_mutex);
}

void q_bump_attempts() {
    xSemaphoreTake(g_q_mutex, portMAX_DELAY);
    if (g_q_count > 0) g_queue[g_q_head].attempts++;
    xSemaphoreGive(g_q_mutex);
}

Op make_op(OpType t) {
    Op op{};
    op.type = t;
    inv_api::make_op_id(op.op_id, sizeof(op.op_id));
    return op;
}

// ---- Op execution -----------------------------------------------------
// Returns true when the op is finished (success OR permanent rejection)
// and should be popped; false to keep it queued for retry.
bool run_op(const Op& op) {
    inv_api::SlotMutResult r{};
    inv_api::JobPickResult jr{};
    inv_api::Status st;
    const char* what = "?";

    switch (op.type) {
        case OpType::Assign:
            what = "assign";
            r  = inv_api::assign_slot(op.slot, op.stock_id, op.op_id);
            st = r.status;
            break;
        case OpType::Pick:
            what = "pick";
            r  = inv_api::pick_slot(op.slot, op.op_id);
            st = r.status;
            break;
        case OpType::JobPick:
            what = "job-pick";
            jr = inv_api::pick_job_item(op.job_id, op.item_idx, op.slot, op.op_id);
            st = jr.status;
            break;
        case OpType::Clear:
            what = "clear";
            r  = inv_api::clear_slot(op.slot, op.text, op.op_id);
            st = r.status;
            break;
        case OpType::Anomaly:
            what = "anomaly";
            r  = inv_api::report_anomaly(op.kind, op.slot, op.text, op.op_id);
            st = r.status;
            break;
        default:
            return true;
    }

    if (st == inv_api::Status::Ok) {
        SR_LOG("op %s slot=%d OK (op_id=%s)", what, op.slot, op.op_id);
        return true;
    }
    if (st == inv_api::Status::BadStatus) {
        // Server actively rejected it (409/404/...). Retrying won't
        // help; the periodic rack reconcile will surface the drift.
        const char* err = (op.type == OpType::JobPick) ? jr.error : r.error;
        SR_LOG("op %s slot=%d REJECTED: %s", what, op.slot, err);
        return true;
    }
    // Transient (no wifi / network / timeout / not configured): retry,
    // but cap attempts so a permanently broken op can't wedge the queue.
    if (op.attempts + 1 >= OP_MAX_ATTEMPTS) {
        SR_LOG("op %s slot=%d DROPPED after %d attempts (%s)",
               what, op.slot, op.attempts + 1, inv_api::status_str(st));
        return true;
    }
    SR_LOG("op %s slot=%d retry (%s, attempt %d)",
           what, op.slot, inv_api::status_str(st), op.attempts + 1);
    return false;
}

// ---- Rack reconciliation (runs on the LVGL task) ----------------------
//
// Server-authoritative and NON-DESTRUCTIVE: reconcile adopts InvenTree's
// view into local state and, at most, raises a LOCAL anomaly. It never
// pushes a clear/pick to the server — physical removals are detected by
// the RS485 event path (apply_slot_change in main.cpp), not here, so a
// flaky slot sensor (or a reel placed via the on-screen dot) can't make
// reconcile dump freshly-placed stock into the pulled bin.
//
// Per slot:
//   server occupied            -> adopt part/qty, mark confirmed. (A reel
//        the HMI just assigned lands here once the assign completes,
//        whether or not the slot sensor reads it.)
//   server empty + local has a CONFIRMED part -> the reel was moved in
//        InvenTree behind our back: drop the local part; if it's still
//        physically here, light it amber + raise "remove it".
//   server empty + local part NOT confirmed   -> a locally-placed reel
//        the server hasn't acknowledged yet (assign pending / unsynced):
//        leave it alone.
//   server empty + local empty -> nothing.
//
// Slots in an active workflow (TARGET/PICKED) are skipped.

bool slot_physically_present(const app::Slot& s) {
    for (int w = 0; w < s.width; ++w) {
        int pix = s.module * app::SlotMap::SLOTS_PER_MODULE + s.mslot + w;
        if (hw_mirror::slot_present(s.port,
                                    pix / app::SlotMap::SLOTS_PER_MODULE,
                                    pix % app::SlotMap::SLOTS_PER_MODULE))
            return true;
    }
    return false;
}

void copy_part(app::Part& dst, const inv_api::Part& src) {
    snprintf(dst.id,   sizeof(dst.id),   "%s", src.id);
    snprintf(dst.name, sizeof(dst.name), "%s", src.name);
    snprintf(dst.pkg,  sizeof(dst.pkg),  "%s", src.pkg);
    snprintf(dst.mfg,  sizeof(dst.mfg),  "%s", src.mfg);
    dst.valid = true;
}

void raise_moved_anomaly(int slot_num) {
    auto& st = app::state();
    app::lock();
    app::Anomaly& a = st.anomaly;
    a.kind = app::AnomalyKind::Added;     // non-latching; removal resolves it
    a.mood = app::AnomalyMood::Warn;
    snprintf(a.title, sizeof(a.title), "Stock moved in InvenTree");
    snprintf(a.message, sizeof(a.message),
             "A reel in this rack was moved to another location in "
             "InvenTree without being removed. Take it out of the lit slot.");
    a.n_detail = 0;
    snprintf(a.detail[a.n_detail].k, sizeof(a.detail[0].k), "Slot");
    snprintf(a.detail[a.n_detail].v, sizeof(a.detail[0].v), "#%d", slot_num);
    a.n_detail++;
    snprintf(a.detail[a.n_detail].k, sizeof(a.detail[0].k), "Action");
    snprintf(a.detail[a.n_detail].v, sizeof(a.detail[0].v), "Remove the reel");
    a.n_detail++;
    st.anomaly_visible = true;
    app::unlock();
    ui::anomaly_modal_open_current();
}

// "Has the server ever confirmed this slot occupied?" — the moved-in-
// InvenTree detection only makes sense for a reel the server previously
// knew about. A freshly-placed reel whose assign is still in flight (or a
// local-only / simulated placement with no stock id) was never confirmed,
// so reconcile must not mistake it for one that was moved away. Touched
// only on the LVGL task (apply_rack_result), so no lock needed.
static bool g_server_confirmed[app::MAX_LOGICAL_SLOTS + 1] = {};

static void set_confirmed(int slot_num, bool v) {
    if (slot_num >= 0 && slot_num <= app::MAX_LOGICAL_SLOTS)
        g_server_confirmed[slot_num] = v;
}
static bool is_confirmed(int slot_num) {
    return slot_num >= 0 && slot_num <= app::MAX_LOGICAL_SLOTS
           && g_server_confirmed[slot_num];
}

void apply_rack_result(void* user) {
    auto* rr = static_cast<inv_api::RackResult*>(user);
    auto& st = app::state();

    // A mutation queued (or in flight) between the worker's fetch and this
    // apply means local state is intentionally ahead of the snapshot — skip
    // rather than reconcile against stale truth. (The worker also gates the
    // fetch on an empty queue; this catches the dispatch-window race.)
    if (pending_ops() > 0) { delete rr; return; }

    const bool have_physical = hw_mirror::valid();
    int n_occ = 0, n_adopt = 0, n_moved = 0, n_clearlocal = 0;

    bool changed = false;
    for (int i = 0; i < rr->n_slots; ++i) {
        const inv_api::RackSlot& srv = rr->slots[i];

        // Hold the lock across the shared-state reads (slot_by_num's scan of
        // st.rack, the slot->state/part/qty reads, and any mutation) so we
        // never tear against the state_store writer task snapshotting under
        // the same mutex. Decisions are computed here, then acted on (logging,
        // dirty-marking, LEDs, anomaly) AFTER unlock to keep the critical
        // section minimal and avoid holding the lock across slow/blocking work
        // (raise_moved_anomaly re-takes app::lock(), so it must run unlocked).
        bool did_adopt = false, did_moved = false, did_clearlocal = false;
        app::lock();
        app::Slot* slot = app::slot_by_num(srv.slot);
        if (!slot) { app::unlock(); continue; }    // not an anchor of a logical slot
        if (slot->state == app::SlotState::TARGET ||
            slot->state == app::SlotState::PICKED) {
            app::unlock();
            continue;                              // active workflow owns it
        }

        const bool present = have_physical && slot_physically_present(*slot);

        if (srv.occupied) {
            n_occ++;
            // Server is the inventory of record; adopt its metadata and
            // remember that it vouches for this slot. We do NOT clear on a
            // physically-absent sensor reading — that's the bug that dumped
            // freshly-assigned reels into the pulled bin.
            set_confirmed(srv.slot, true);
            if (!slot->part.valid || slot->qty != srv.qty ||
                strcmp(slot->part.id, srv.part.id) != 0) {
                copy_part(slot->part, srv.part);
                slot->qty   = srv.qty;
                slot->state = app::SlotState::OCCUPIED;
                did_adopt = true;
            }
        } else if (slot->part.valid && is_confirmed(srv.slot)) {
            // Server PREVIOUSLY confirmed this reel and now reports the slot
            // empty → it was moved in InvenTree behind our back. Drop the
            // local part; if the reel is still physically here, light it and
            // tell the user to remove it (user-stories.md). Confirmed-gate
            // keeps this off a just-placed/unsynced reel.
            set_confirmed(srv.slot, false);
            slot->part.valid = false;
            slot->qty        = 0;
            slot->state      = present ? app::SlotState::WARN : app::SlotState::EMPTY;
            if (present) did_moved = true;
            else         did_clearlocal = true;
        }
        // server empty + local part NOT confirmed: a locally-placed reel the
        // server hasn't acknowledged yet (assign pending / unsynced). Leave
        // it — the assign op syncs it, then a later reconcile confirms it.
        app::unlock();

        // Side effects, outside the lock.
        if (did_adopt) {
            state_store::mark_slot_dirty(srv.slot);
            SR_LOG("reconcile slot=%d adopt %s qty=%d (present=%d)",
                   srv.slot, srv.part.id, srv.qty, present);
            n_adopt++;
            changed = true;
        } else if (did_moved) {
            state_store::mark_slot_dirty(srv.slot);
            leds::light_slot(srv.slot, 0xF5, 0x9E, 0x0B);   // amber: remove me
            raise_moved_anomaly(srv.slot);
            // Record the move in InvenTree's anomaly log too (mirrors the
            // hardware-removal path in main.cpp), not just the local modal.
            inv_sync::queue_anomaly("moved", srv.slot, "stock moved in InvenTree, still present");
            SR_LOG("reconcile slot=%d MOVED in InvenTree, still present -> remove", srv.slot);
            n_moved++;
            changed = true;
        } else if (did_clearlocal) {
            state_store::mark_slot_dirty(srv.slot);
            SR_LOG("reconcile slot=%d gone from InvenTree and absent -> clear local", srv.slot);
            n_clearlocal++;
            changed = true;
        }
    }

    SR_LOG("reconcile: %d slots (%d server-occupied) adopt=%d moved=%d cleared-local=%d phys=%d",
           rr->n_slots, n_occ, n_adopt, n_moved, n_clearlocal, have_physical);

    if (changed && (ui::current() == ui::Screen::Home ||
                    ui::current() == ui::Screen::View ||
                    ui::current() == ui::Screen::RackGrid)) {
        ui::rebuild_current();
    }
    delete rr;
}

// ---- Pick jobs apply (LVGL task) --------------------------------------

void apply_jobs_result(void* user) {
    auto* jr = static_cast<inv_api::PickJobsResult*>(user);
    auto& st = app::state();

    // Never reshape the job list under an active job: indices would
    // shift mid-pick. The list refreshes on the next screen entry. Read
    // active_pick_idx under the lock -- it is mutated by other state
    // mutators (e.g. mock_start_pick) holding the same mutex.
    app::lock();
    if (st.active_pick_idx >= 0) {
        app::unlock();
        delete jr;
        return;
    }

    st.n_pick_jobs = 0;
    for (int i = 0; i < jr->n_jobs && st.n_pick_jobs < app::MAX_PICK_JOBS; ++i) {
        const inv_api::PickJobInfo& src = jr->jobs[i];
        if (strcmp(src.job_status, "done") == 0) continue;   // finished jobs hide
        app::PickJob& dst = st.pick_jobs[st.n_pick_jobs++];
        snprintf(dst.id,   sizeof(dst.id),   "%s", src.id);
        snprintf(dst.name, sizeof(dst.name), "%s", src.name);
        // ISO "2026-06-11T14:02:00" -> "14:02" for the row meta.
        if (strlen(src.requested) >= 16)
            snprintf(dst.requested, sizeof(dst.requested), "%.5s", src.requested + 11);
        else
            snprintf(dst.requested, sizeof(dst.requested), "%s", src.requested);
        snprintf(dst.status, sizeof(dst.status), "%s", src.job_status);
        dst.n_items = 0;
        for (int k = 0; k < src.n_items && dst.n_items < 16; ++k) {
            app::PickItem& it = dst.items[dst.n_items++];
            snprintf(it.part_id,   sizeof(it.part_id),   "%s", src.items[k].part_id);
            snprintf(it.part_name, sizeof(it.part_name), "%s", src.items[k].part_name);
            it.qty      = src.items[k].qty;
            it.picked   = src.items[k].picked;
            it.slot_num = 0;     // resolved against the local rack below
        }
    }

    // Still under the lock: resolve_pick_locations() scans st.rack (via
    // find_part) and writes st.pick_jobs[].items[].slot_num, so it must run
    // inside the same critical section (matches mock_start_pick()).
    for (int i = 0; i < st.n_pick_jobs; ++i)
        app::resolve_pick_locations(st.pick_jobs[i]);
    app::unlock();

    state_store::mark_jobs_dirty();
    if (ui::current() == ui::Screen::PickList) ui::rebuild_current();
    delete jr;
}

// ---- Worker -----------------------------------------------------------

int physical_slot_count() {
    int total = 0;
    for (int p = 0; p < hw_mirror::N_PORTS; ++p)
        total += hw_mirror::module_count(p) * app::SlotMap::SLOTS_PER_MODULE;
    return total > 0 ? total : 64;   // sane default before topology known
}

void set_online(bool on) {
    bool was = g_online;
    g_online = on;
    if (on) g_last_health_ok = millis();
    if (was != on) {
        SR_LOG("%s InvenTree", on ? "ONLINE ->" : "OFFLINE ->");
        // app_state.online drives the status bar pill. Cheap bool write
        // marshalled to the LVGL task with the existing dispatcher.
        ui::dispatch_on_lvgl([](void* v) {
            app::state().online = (v != nullptr);
            ui::rebuild_current();
        }, on ? (void*)1 : nullptr);
    }
}

void worker(void*) {
    uint32_t next_health = 0;
    uint32_t next_rack   = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        if (!inv_api::configured()) {
            g_registered = false;
            set_online(false);
            continue;
        }

        const uint32_t now = millis();

        // Health (+ registration on transition to online).
        if ((int32_t)(now - next_health) >= 0) {
            inv_api::HealthResult h = inv_api::health();
            const bool ok = (h.status == inv_api::Status::Ok);
            set_online(ok);
            next_health = now + (ok ? HEALTH_OK_IVL_MS : HEALTH_BAD_IVL_MS);
            if (ok && !g_registered) {
                char op_id[40];
                inv_api::make_op_id(op_id, sizeof(op_id));
                auto r = inv_api::register_rack(physical_slot_count(), op_id);
                if (r.status == inv_api::Status::Ok) {
                    g_registered = true;
                    g_want_rack  = true;   // boot sync right after registration
                    SR_LOG("registered rack (%d slots)", physical_slot_count());
                } else {
                    SR_LOG("register FAILED: %s", r.error);
                }
            }
        }

        if (!g_online) continue;

        // Drain the mutation queue first: ordering matters (an assign
        // queued before a pick of the same slot must land first).
        Op op;
        while (q_peek(op)) {
            if (run_op(op)) {
                q_pop();
            } else {
                q_bump_attempts();
                break;            // transient failure: retry next tick
            }
        }

        // Rack reconcile: periodic + on demand — but only when the queue
        // drained fully this tick. A non-empty queue means an assign/pick
        // hasn't landed, so the server snapshot is stale w.r.t. local intent
        // and reconciling would raise false anomalies. Leaving the timer/
        // flag as-is retries next tick once the op clears.
        if ((g_want_rack || (int32_t)(now - next_rack) >= 0)
            && pending_ops() == 0) {
            g_want_rack = false;
            next_rack   = now + RACK_IVL_MS;
            auto* rr = new (std::nothrow) inv_api::RackResult();
            if (rr) {
                inv_api::get_rack(*rr);
                if (rr->status == inv_api::Status::Ok) {
                    ui::dispatch_on_lvgl(apply_rack_result, rr);
                } else {
                    SR_LOG("GET /rack FAILED: %s", rr->error);
                    delete rr;
                }
            }
        }

        // Pick jobs: on demand only.
        if (g_want_jobs) {
            g_want_jobs = false;
            auto* jr = new (std::nothrow) inv_api::PickJobsResult();
            if (jr) {
                inv_api::get_pickjobs(*jr);
                if (jr->status == inv_api::Status::Ok) {
                    SR_LOG("GET /pickjobs OK (%d jobs)", jr->n_jobs);
                    ui::dispatch_on_lvgl(apply_jobs_result, jr);
                } else {
                    SR_LOG("GET /pickjobs FAILED: %s", jr->error);
                    delete jr;
                }
            }
        }
    }
}

} // anonymous namespace

// ---- Public -----------------------------------------------------------

void start() {
    if (g_q_mutex) return;   // already started
    g_q_mutex = xSemaphoreCreateMutex();
    // 8 KB stack: TLS + HTTPClient + the /rack JSON parse. Pinned to
    // APP_CPU at priority 1 (see header).
    xTaskCreatePinnedToCore(worker, "inv-sync", 8 * 1024, nullptr, 1,
                            nullptr, APP_CPU_NUM);
}

void queue_assign(int slot_num, int stock_id) {
    Op op = make_op(OpType::Assign);
    op.slot = slot_num;
    op.stock_id = stock_id;
    SR_LOG("queue assign slot=%d stock=%d", slot_num, stock_id);
    q_push(op);
}

void queue_pick(int slot_num) {
    Op op = make_op(OpType::Pick);
    op.slot = slot_num;
    SR_LOG("queue pick slot=%d", slot_num);
    q_push(op);
}

void queue_job_pick(const char* job_id, int item_idx, int slot_num) {
    Op op = make_op(OpType::JobPick);
    snprintf(op.job_id, sizeof(op.job_id), "%s", job_id ? job_id : "");
    op.item_idx = item_idx;
    op.slot = slot_num;
    SR_LOG("queue job-pick job=%s item=%d slot=%d", op.job_id, item_idx, slot_num);
    q_push(op);
}

void queue_clear(int slot_num, const char* reason) {
    Op op = make_op(OpType::Clear);
    op.slot = slot_num;
    snprintf(op.text, sizeof(op.text), "%s", reason ? reason : "");
    SR_LOG("queue clear slot=%d (%s)", slot_num, op.text);
    q_push(op);
}

void queue_anomaly(const char* kind, int slot_num, const char* detail) {
    Op op = make_op(OpType::Anomaly);
    op.slot = slot_num;
    snprintf(op.kind, sizeof(op.kind), "%s", kind ? kind : "removed");
    snprintf(op.text, sizeof(op.text), "%s", detail ? detail : "");
    SR_LOG("queue anomaly slot=%d kind=%s (%s)", slot_num, op.kind, op.text);
    q_push(op);
}

void request_jobs_refresh(bool force) {
    // Throttled: build_pick_list() requests on every (re)build and the
    // apply path rebuilds the screen, so an unthrottled request would
    // ping-pong fetch->rebuild->fetch forever. 10 s keeps the list
    // live-ish while the user sits on the screen without hammering.
    // A manual Refresh button passes force=true to bypass the throttle.
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (!force && last_ms != 0 && (now - last_ms) < 10000) return;
    last_ms = now;
    g_want_jobs = true;
    SR_LOG("jobs refresh requested%s", force ? " (forced)" : "");
}

void request_rack_refresh() { g_want_rack = true; }

bool online() {
    if (!g_online) return false;
    return (millis() - g_last_health_ok) < ONLINE_WINDOW_MS;
}

int pending_ops() {
    if (!g_q_mutex) return 0;
    xSemaphoreTake(g_q_mutex, portMAX_DELAY);
    int n = g_q_count;
    xSemaphoreGive(g_q_mutex);
    return n;
}

} // namespace inv_sync
