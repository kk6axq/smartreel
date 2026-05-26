// =====================================================================
//  Persistent runtime state on SD.
//
//  Three files at the SD root, each owning one slice of app::State:
//
//      /sdcard/state.json     -- per-slot occupancy + part assignments.
//                                Rewritten as a whole every save.
//      /sdcard/jobs.json      -- pick queue + per-job progress.
//                                Rewritten as a whole every save.
//      /sdcard/anomalies.jsonl -- append-only event log, one JSON
//                                object per line. Easy to inspect on
//                                the host machine with grep / jq.
//
//  Threading
//      Mutators on the LVGL task call mark_*_dirty() / log_anomaly()
//      after updating app::state(). Those calls just set a bit and
//      signal the writer semaphore -- they do NOT block on I/O.
//
//      A dedicated writer task (state_store_writer, priority 1 on
//      PRO_CPU) wakes on the semaphore, debounces 250 ms to coalesce
//      bursts, then snapshots app::state() under the app::lock()
//      mutex, releases the lock, and serialises the snapshot to JSON
//      at leisure before doing the atomic SD write.
//
//      So even a burst of 30 RS485 events per second produces at
//      most ~4 SD writes per second, and the LVGL task never blocks
//      on the SD card.
//
//  On boot, load() reads whatever's on disk into app::state(). If
//  any file is missing or corrupt the corresponding fields keep their
//  defaults (which today come from the mock-data seed in app_state).
// =====================================================================
#pragma once

#include "ui/app_state.h"

#include <stdbool.h>

namespace state_store {

// Read JSON files from SD into app::state(). Returns true if at
// least one of state.json / jobs.json successfully loaded. Caller
// should check this and decide whether to keep the mock-data seed.
bool load();

// Start the writer task. Idempotent. Returns false on alloc failure.
bool start_writer();

// Mark a slot's persistent fields dirty. Triggers a state.json
// write after the debounce. `slot_num` is 1..N_SLOTS; out-of-range
// silently ignored.
void mark_slot_dirty(int slot_num);

// Convenience: mark the whole rack dirty (e.g. after a wholesale
// load). Same effect as marking each slot.
void mark_all_slots_dirty();

// Mark the pick queue dirty. Either the active index, an item's
// picked flag, or the queue contents changed.
void mark_jobs_dirty();

// Append one anomaly record to anomalies.jsonl. Non-blocking: the
// record is copied into a small in-RAM ring; the writer flushes it
// on its next wake.
void log_anomaly(const app::Anomaly& a);

// Stats for diagnostics.
struct Stats {
    uint32_t state_writes;
    uint32_t jobs_writes;
    uint32_t anomalies_written;
    uint32_t write_errors;
};
const Stats& stats();

} // namespace state_store
