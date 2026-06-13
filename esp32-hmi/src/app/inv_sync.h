// =====================================================================
//  InvenTree synchronisation layer.
//
//  Owns ALL background traffic to the SmartReel plugin so UI code never
//  blocks on the network:
//
//    - health poll      -> drives app_state.online + connectivity UX
//    - rack/register    -> once after first successful contact
//    - GET /rack        -> periodic reconciliation (user-stories.md:
//                          stock moved in InvenTree without a physical
//                          removal lights the slot + instructs removal)
//    - GET /pickjobs    -> on demand (Pick screen)
//    - op queue         -> assign / pick / job-pick / clear / anomaly
//                          mutations enqueued from the LVGL task and
//                          executed here with retries. Server-side
//                          op_id idempotency makes retries safe.
//
//  Threading: one worker task pinned to APP_CPU at priority 1 (PRO_CPU
//  runs the RGB DMA; see memory hmi-cpu-pinning). Results are applied
//  to app_state via ui::dispatch_on_lvgl.
// =====================================================================
#pragma once

#include <stdint.h>

namespace inv_sync {

// Create the worker task. Call once from setup() after inv_api::init().
void start();

// ---- Mutation queue (call from the LVGL task) -----------------------
// Each op gets a fresh op_id at enqueue time and is retried with backoff
// until the server accepts it (2xx) or permanently rejects it (4xx).
// Queue overflow drops the op with a serial log -- the next rack
// reconcile pass will surface any resulting drift.
void queue_assign (int slot_num, int stock_id);
void queue_pick   (int slot_num);                     // whole-reel -> staging
void queue_job_pick(const char* job_id, int item_idx, int slot_num);
void queue_clear  (int slot_num, const char* reason);
void queue_anomaly(const char* kind, int slot_num, const char* detail);

// ---- On-demand refreshes (async; results land via dispatch) ---------
// request_jobs_refresh is throttled (it's called on every Pick-screen
// rebuild); pass force=true from a manual Refresh button to bypass that.
void request_jobs_refresh(bool force = false);
void request_rack_refresh();

// ---- Status for the UI ----------------------------------------------
// True when the last health check succeeded recently. This is the
// "may I pick?" gate (user-stories.md: no picking while InvenTree is
// unreachable).
bool online();

// Number of queued-but-unacknowledged mutations (status display).
int  pending_ops();

} // namespace inv_sync
