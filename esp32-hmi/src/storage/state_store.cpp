#include "storage/state_store.h"
#include "storage/sdcard.h"

#include <ArduinoJson.h>
#include <esp32-hal-log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdio.h>

namespace state_store {

// ---- File paths --------------------------------------------------
static constexpr const char* PATH_STATE     = "/sdcard/state.json";
static constexpr const char* PATH_JOBS      = "/sdcard/jobs.json";
static constexpr const char* PATH_ANOMALIES = "/sdcard/anomalies.jsonl";

// ---- Dirty bits, signalled to the writer task --------------------
static constexpr uint32_t DIRTY_STATE = 1u << 0;
static constexpr uint32_t DIRTY_JOBS  = 1u << 1;
static volatile uint32_t  s_dirty_flags = 0;

// Writer task plumbing
static SemaphoreHandle_t  s_writer_sem  = nullptr;
static SemaphoreHandle_t  s_pending_mtx = nullptr;
static TaskHandle_t       s_writer_task = nullptr;
static Stats              s_stats       = {};

// Pending anomaly lines (mutexed because producers can be on any task)
static constexpr int  PEND_MAX      = 16;
static constexpr int  PEND_LINE_LEN = 240;
static char           s_pending[PEND_MAX][PEND_LINE_LEN];
static int            s_pend_count  = 0;

const Stats& stats() { return s_stats; }

// ===================================================================
//  (Slot occupancy is live/derived; only part/qty contents are persisted,
//   keyed by logical slot number -- see write/load_state_json.)
// ===================================================================

// ===================================================================
//  Save helpers (run on the writer task, given a snapshot)
// ===================================================================
static bool write_state_json(const app::State& s) {
    JsonDocument doc;
    doc["version"] = 2;
    // Rack contents keyed by LOGICAL slot number (presence is live, so we
    // persist only the part/qty assignment for occupied logical slots).
    auto rack = doc["rack"].to<JsonArray>();
    for (int i = 0; i < s.n_rack; ++i) {
        const auto& slot = s.rack[i];
        if (!slot.part.valid) continue;
        auto o = rack.add<JsonObject>();
        o["num"]     = slot.slot;
        o["part_id"] = slot.part.id;
        o["name"]    = slot.part.name;
        o["pkg"]     = slot.part.pkg;
        o["mfg"]     = slot.part.mfg;
        o["qty"]     = slot.qty;
    }
    // Worst case ~9 KB. Use a heap buffer so the writer stack stays small.
    static constexpr size_t BUF_SZ = 12 * 1024;
    char* buf = static_cast<char*>(heap_caps_malloc(BUF_SZ, MALLOC_CAP_8BIT));
    if (!buf) { s_stats.write_errors++; return false; }

    size_t n = serializeJson(doc, buf, BUF_SZ);
    bool ok = (n > 0 && n < BUF_SZ) &&
              sdcard::write_file_atomic(PATH_STATE, buf, n);
    heap_caps_free(buf);
    if (!ok) { s_stats.write_errors++; return false; }
    s_stats.state_writes++;
    return true;
}

static bool write_jobs_json(const app::State& s) {
    JsonDocument doc;
    doc["version"] = 1;
    doc["active"]  = s.active_pick_idx;
    auto queue = doc["queue"].to<JsonArray>();
    for (int i = 0; i < s.n_pick_jobs; ++i) {
        const auto& j = s.pick_jobs[i];
        auto jo = queue.add<JsonObject>();
        jo["id"]        = j.id;
        jo["name"]      = j.name;
        jo["requested"] = j.requested;
        auto items = jo["items"].to<JsonArray>();
        for (int k = 0; k < j.n_items; ++k) {
            const auto& it = j.items[k];
            auto io = items.add<JsonObject>();
            io["part_id"] = it.part_id;
            io["qty"]     = it.qty;
            io["slot"]    = it.slot_num;
            io["picked"]  = it.picked;
        }
    }
    static constexpr size_t BUF_SZ = 8 * 1024;
    char* buf = static_cast<char*>(heap_caps_malloc(BUF_SZ, MALLOC_CAP_8BIT));
    if (!buf) { s_stats.write_errors++; return false; }

    size_t n = serializeJson(doc, buf, BUF_SZ);
    bool ok = (n > 0 && n < BUF_SZ) &&
              sdcard::write_file_atomic(PATH_JOBS, buf, n);
    heap_caps_free(buf);
    if (!ok) { s_stats.write_errors++; return false; }
    s_stats.jobs_writes++;
    return true;
}

// Append any pending lines to anomalies.jsonl in one open/close.
static bool append_pending_anomalies() {
    if (!sdcard::mounted()) return false;
    // Snapshot under mutex. `local` is static (not on the stack): at
    // ~3.8 KB it would otherwise blow the writer task's stack when this
    // runs in the same iteration as a state/jobs write (the combined
    // FATFS call depth tripped the stack canary). Safe to be static --
    // only the single writer task ever calls this.
    static char local[PEND_MAX][PEND_LINE_LEN];
    int   n = 0;
    xSemaphoreTake(s_pending_mtx, portMAX_DELAY);
    n = s_pend_count;
    if (n > 0) {
        memcpy(local, s_pending, sizeof(char) * n * PEND_LINE_LEN);
        s_pend_count = 0;
    }
    xSemaphoreGive(s_pending_mtx);
    if (n == 0) return true;

    FILE* f = fopen(PATH_ANOMALIES, "ab");
    if (!f) {
        log_e("anomalies.jsonl open failed");
        s_stats.write_errors++;
        return false;
    }
    for (int i = 0; i < n; ++i) {
        const size_t len = strnlen(local[i], PEND_LINE_LEN);
        if (len) fwrite(local[i], 1, len, f);
    }
    fclose(f);
    s_stats.anomalies_written += n;
    return true;
}

// ===================================================================
//  Writer task
// ===================================================================
static void writer_task(void* /*arg*/) {
    constexpr TickType_t debounce = pdMS_TO_TICKS(250);

    // Snapshot buffer in PSRAM. ~17 KB; we only need one at a time.
    app::State* snap = static_cast<app::State*>(
        heap_caps_malloc(sizeof(app::State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!snap) {
        log_e("state_store: snapshot alloc failed; writer exiting");
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        // Block until somebody asks us to do work.
        xSemaphoreTake(s_writer_sem, portMAX_DELAY);

        // Coalesce: hold off long enough for a burst to settle, then
        // drain any extra signals that arrived during the wait.
        vTaskDelay(debounce);
        while (xSemaphoreTake(s_writer_sem, 0) == pdTRUE) { /* drain */ }

        // Atomically read + clear the dirty flag set.
        const uint32_t flags = __atomic_exchange_n(&s_dirty_flags, 0u, __ATOMIC_ACQ_REL);

        // Take a tight snapshot of app_state under the app::lock()
        // mutex. The lock is held only for the memcpy (~tens of us);
        // serialisation happens lock-free against the local copy.
        if (flags & (DIRTY_STATE | DIRTY_JOBS)) {
            app::lock();
            memcpy(snap, &app::state(), sizeof(app::State));
            app::unlock();

            if (flags & DIRTY_STATE) write_state_json(*snap);
            if (flags & DIRTY_JOBS)  write_jobs_json(*snap);
        }

        // Anomaly lines: written from the s_pending ring regardless
        // of dirty flags (they're signalled via the same semaphore).
        append_pending_anomalies();
    }
}

bool start_writer() {
    if (s_writer_task) return true;

    s_writer_sem  = xSemaphoreCreateBinary();
    s_pending_mtx = xSemaphoreCreateMutex();
    if (!s_writer_sem || !s_pending_mtx) return false;

    // Pinned to APP_CPU (never PRO_CPU): PRO_CPU runs the RGB LCD DMA
    // refresh. This task snapshots app::state() out of PSRAM and does
    // bursty SD writes; on PRO_CPU that contends for PSRAM bandwidth and
    // starves the DMA bounce buffer (visible tearing). Priority 1, below
    // the LVGL task (2).
    BaseType_t ok = xTaskCreatePinnedToCore(
        writer_task, "state-writer",
        8 * 1024, nullptr,          // FATFS write depth + JSON; headroom
        /*priority=*/1, &s_writer_task,
        APP_CPU_NUM);
    return ok == pdPASS;
}

// ===================================================================
//  Public mutator API (call from any task)
// ===================================================================
static inline void signal_writer() {
    if (s_writer_sem) xSemaphoreGive(s_writer_sem);
}

void mark_slot_dirty(int /*slot_num*/) {
    __atomic_fetch_or(&s_dirty_flags, DIRTY_STATE, __ATOMIC_RELEASE);
    signal_writer();
}

void mark_all_slots_dirty() {
    __atomic_fetch_or(&s_dirty_flags, DIRTY_STATE, __ATOMIC_RELEASE);
    signal_writer();
}

void mark_jobs_dirty() {
    __atomic_fetch_or(&s_dirty_flags, DIRTY_JOBS, __ATOMIC_RELEASE);
    signal_writer();
}

void log_anomaly(const app::Anomaly& a) {
    // Build the JSON line into a local buffer first.
    char line[PEND_LINE_LEN];
    JsonDocument doc;
    doc["ts"]    = (uint32_t)esp_log_timestamp();
    doc["kind"]  =
        (a.kind == app::AnomalyKind::Removed)  ? "reel_removed" :
        (a.kind == app::AnomalyKind::Added)    ? "reel_added"   :
        (a.kind == app::AnomalyKind::Divider)  ? "divider"      : "unknown";
    doc["title"] = a.title;
    doc["msg"]   = a.message;
    // Optional: dump detail rows as a nested object.
    if (a.n_detail > 0) {
        auto det = doc["detail"].to<JsonObject>();
        for (int i = 0; i < a.n_detail; ++i) {
            det[a.detail[i].k] = a.detail[i].v;
        }
    }
    size_t n = serializeJson(doc, line, sizeof(line) - 2);
    if (n == 0 || n >= sizeof(line) - 2) return;
    line[n++] = '\n';
    line[n]   = 0;

    xSemaphoreTake(s_pending_mtx, portMAX_DELAY);
    if (s_pend_count < PEND_MAX) {
        memcpy(s_pending[s_pend_count], line, n + 1);
        s_pend_count++;
    } else {
        log_w("anomalies.jsonl pending ring full; dropping line");
    }
    xSemaphoreGive(s_pending_mtx);
    signal_writer();
}

// ===================================================================
//  Boot-time load
// ===================================================================
static bool load_state_json(app::State& s) {
    if (!sdcard::mounted()) return false;
    size_t fsz = sdcard::file_size(PATH_STATE);
    if (fsz == 0 || fsz > 16 * 1024) return false;

    char* buf = static_cast<char*>(heap_caps_malloc(fsz + 1, MALLOC_CAP_8BIT));
    if (!buf) return false;
    size_t got = sdcard::read_file(PATH_STATE, buf, fsz);
    buf[got] = 0;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, got);
    heap_caps_free(buf);
    if (err) {
        log_w("state.json parse failed: %s", err.c_str());
        return false;
    }
    // Contents are keyed by logical number and applied onto the already-
    // built dynamic rack (rebuild_rack() must have run first). A saved
    // number with no matching present slot is simply ignored.
    JsonArrayConst rack = doc["rack"].as<JsonArrayConst>();
    int applied = 0;
    for (JsonObjectConst o : rack) {
        if (!o["part_id"].is<const char*>()) continue;
        int num = o["num"] | 0;
        app::Slot* slot = app::slot_by_num(num);
        if (!slot) continue;
        slot->part.valid = true;
        snprintf(slot->part.id,   sizeof(slot->part.id),   "%s", o["part_id"].as<const char*>());
        snprintf(slot->part.name, sizeof(slot->part.name), "%s", o["name"] | "");
        snprintf(slot->part.pkg,  sizeof(slot->part.pkg),  "%s", o["pkg"]  | "");
        snprintf(slot->part.mfg,  sizeof(slot->part.mfg),  "%s", o["mfg"]  | "");
        slot->qty = o["qty"] | 0;
        if (slot->state == app::SlotState::EMPTY) slot->state = app::SlotState::OCCUPIED;
        applied++;
    }
    log_i("state.json loaded (%d slot contents)", applied);
    return true;
}

// NOTE: jobs.json is write-only now. Pick jobs are live from InvenTree
// (GET /pickjobs via inv_sync); the on-disk copy is kept only as a
// debugging/forensic snapshot and is never read back into app_state
// (doing so resurrected stale placeholder jobs). See load().

bool load() {
    if (!sdcard::mounted()) {
        log_i("state_store: no SD; mock data retained");
        return false;
    }
    // Mutate app::state() under the lock. Slot contents are restored from
    // SD; pick jobs are NOT — they're live from InvenTree (GET /pickjobs
    // via inv_sync). Reloading jobs.json here resurrected stale placeholder
    // jobs (and a stale active_pick_idx that blocked the live refresh), so
    // the jobs cache is intentionally not read back.
    app::lock();
    bool a = load_state_json(app::state());
    app::unlock();
    return a;
}

} // namespace state_store
