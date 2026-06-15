// PICK list + active.
//
// The job list comes from InvenTree (GET /pickjobs via inv_sync); the
// SD jobs.json is only a boot-time cache. Picking is blocked while the
// server is unreachable (user-stories.md, Internet connectivity) so the
// rack can't drift out of sync.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "ui/notify.h"
#include "storage/state_store.h"
#include "app/leds.h"
#include "app/beeper.h"
#include "app/inv_sync.h"
#include "net/inv_api.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

namespace ui::screens {

using namespace theme;

// ---- pick-list ----------------------------------------------------
struct StartCtx { int idx; };
static void on_start(lv_event_t* e) {
    auto* c = static_cast<StartCtx*>(lv_event_get_user_data(e));
    if (!c) return;
    if (!inv_sync::online()) return;     // picking is online-only
    app::mock_start_pick(c->idx);
    leds::light_target_slots();      // fire and forget; ok if Core absent
    ui::navigate(Screen::PickActive);
}
static void free_start_ctx(lv_event_t* e) {
    delete static_cast<StartCtx*>(lv_event_get_user_data(e));
}

// Manual force-refresh: bypasses the throttle. The result lands via
// dispatch and rebuilds this screen with the live list. The throttled
// auto-refresh gave no feedback, so a tap could look dead (review item 8):
// acknowledge the tap immediately with a toast + tick, then force-fetch.
static void on_jobs_refresh(lv_event_t*) {
    beeper::click();
    if (!inv_sync::online()) {
        ui::toast("Offline -- can't refresh jobs", ui::NotifyMood::Warn);
        return;
    }
    ui::toast("Refreshing jobs...", ui::NotifyMood::Info, 1500);
    inv_sync::request_jobs_refresh(/*force=*/true);
}

void build_pick_list(lv_obj_t* body) {
    // Kick a (throttled) refresh every time the list is (re)shown; the
    // result lands via dispatch and rebuilds this screen.
    inv_sync::request_jobs_refresh();

    const bool online = inv_sync::online();

    lv_obj_t* sc = row_scroller(body);

    if (!online) {
        banner(sc, inv_api::configured()
                       ? "InvenTree is unreachable - picking is disabled until "
                         "the connection comes back."
                       : "InvenTree is not configured - set it up in Settings > "
                         "Network to enable picking.",
               /*warn=*/true);
    }

    // Refresh row (always present, so the live list can be re-pulled even
    // when it's currently empty).
    {
        lv_obj_t* hdr = lv_obj_create(sc);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_END,
                                   LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
        button(hdr, "Refresh", BtnKind::Default, on_jobs_refresh);
    }

    auto& st = app::state();
    if (st.n_pick_jobs == 0) {
        empty_state(sc, online ? "No pick jobs. Send one from a Build Order "
                                 "in InvenTree, then Refresh."
                               : "No pick jobs (offline).");
        return;
    }
    for (int i = 0; i < st.n_pick_jobs; ++i) {
        auto& j = st.pick_jobs[i];
        RowOpts ro; ro.use_accent_stripe = true;
        lv_obj_t* row = row_make(sc, ro);

        row_add_slot_num(row, j.id);

        char meta[80];
        const bool partial = strcmp(j.status, "partial") == 0;
        snprintf(meta, sizeof(meta), "%d parts  requested %s%s",
                 j.n_items, j.requested,
                 partial ? "  -  partially picked" : "");
        row_add_main_two_line(row, j.name, meta);

        auto* ctx = new StartCtx{ i };
        lv_obj_t* b = row_add_button(row, partial ? "Resume" : "Start",
                                     on_start, ctx,
                                     /*muted=*/false, /*success=*/false,
                                     /*disabled=*/!online);
        lv_obj_add_event_cb(b, free_start_ctx, LV_EVENT_DELETE, ctx);
    }
}

// ---- pick-active --------------------------------------------------
// Right-aligned per-line status (review item 13: the confusing "Picked"
// button is gone; a line just goes green when its reel is physically
// pulled). Picking is driven by the reel-presence sensor in main.cpp's
// apply_slot_change(), not a tap, so there's no per-row action button.
static void row_add_status(lv_obj_t* row, const char* text, lv_color_t col) {
    lv_obj_t* l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_min_width(l, 160, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(l, 160);
}

// Cancel the active job: revert any still-TARGET slots to OCCUPIED
// (the reels never moved), drop active_pick_idx, persist, clear all
// reel LEDs, and navigate back to the pick-list screen.
static void cancel_active_job(lv_event_t*) {
    auto& st = app::state();
    if (st.active_pick_idx < 0) {
        ui::navigate(Screen::PickList);
        return;
    }
    app::lock();
    for (int i = 0; i < st.n_rack; ++i) {
        if (st.rack[i].state == app::SlotState::TARGET) {
            st.rack[i].state = app::SlotState::OCCUPIED;
        }
        // PICKED stays as PICKED (real physical removal during the job).
    }
    st.active_pick_idx = -1;
    app::unlock();
    state_store::mark_jobs_dirty();
    state_store::mark_all_slots_dirty();
    leds::clear_all();
    ui::navigate(Screen::PickList);
}

// Complete the job once every item is picked: the picked reels are
// physically gone (each was reported to InvenTree as it was pulled, so
// the server job is already "done"). Free those slots locally, drop the
// active job, clear LEDs, and return to the refreshed list.
static void complete_active_job(lv_event_t*) {
    auto& st = app::state();
    if (st.active_pick_idx < 0) { ui::navigate(Screen::PickList); return; }
    app::lock();
    for (int i = 0; i < st.n_rack; ++i) {
        if (st.rack[i].state == app::SlotState::PICKED) {
            st.rack[i].state      = app::SlotState::EMPTY;
            st.rack[i].part.valid = false;
            st.rack[i].qty        = 0;
        }
    }
    st.active_pick_idx = -1;
    app::unlock();
    state_store::mark_jobs_dirty();
    state_store::mark_all_slots_dirty();
    leds::clear_all();
    inv_sync::request_jobs_refresh(/*force=*/true);   // done job drops off the list
    beeper::ok();
    ui::toast("Pick job complete", ui::NotifyMood::Success);
    ui::navigate(Screen::PickList);
}

void build_pick_active(lv_obj_t* body) {
    auto& st = app::state();
    if (st.active_pick_idx < 0) {
        ui::navigate(Screen::PickList);
        return;
    }
    auto& j = st.pick_jobs[st.active_pick_idx];

    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_gap(body, 0, 0);

    // Header (job name + progress + bar)
    lv_obj_t* head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(head, color::surface(), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(head, color::border(), 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_pad_hor(head, 12, 0);
    lv_obj_set_style_pad_ver(head, 8, 0);
    lv_obj_set_style_pad_gap(head, 6, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* row1 = lv_obj_create(head);
    lv_obj_remove_style_all(row1);
    lv_obj_set_size(row1, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* nm = lv_label_create(row1);
    lv_label_set_text(nm, j.name);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(nm, color::text(), 0);

    int done = app::pick_job_done_count(j);
    char prog[16]; snprintf(prog, sizeof(prog), "%d / %d", done, j.n_items);
    lv_obj_t* pl = lv_label_create(row1);
    lv_label_set_text(pl, prog);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(pl, color::text_muted(), 0);

    int pct = j.n_items > 0 ? (done * 100 / j.n_items) : 0;
    progress_bar(head, pct);

    // List of items, ordered by slot number so the operator walks the rack
    // left-to-right (review item 12). Located lines (slot > 0) sort ascending;
    // unlocated lines ("not in rack") sink to the bottom.
    int order[16];
    const int n = j.n_items < 16 ? j.n_items : 16;
    for (int i = 0; i < n; ++i) order[i] = i;
    auto key = [&](int idx) {
        int s = j.items[idx].slot_num;
        return s > 0 ? s : INT_MAX;
    };
    for (int a = 1; a < n; ++a) {            // insertion sort (n <= 16)
        int v = order[a], k = key(v), b = a - 1;
        while (b >= 0 && key(order[b]) > k) { order[b + 1] = order[b]; --b; }
        order[b + 1] = v;
    }

    lv_obj_t* sc = row_scroller(body);
    lv_obj_set_flex_grow(sc, 1);
    for (int o = 0; o < n; ++o) {
        auto& it = j.items[order[o]];

        RowOpts ro;
        if (it.picked)            ro.stripe_state = app::SlotState::PICKED;
        else if (it.slot_num > 0) ro.stripe_state = app::SlotState::TARGET;
        else                      ro.stripe_state = app::SlotState::ERROR;

        lv_obj_t* row = row_make(sc, ro);

        char tag[12];
        if (it.slot_num > 0) snprintf(tag, sizeof(tag), "#%d", it.slot_num);
        else                 snprintf(tag, sizeof(tag), "-");
        row_add_slot_num(row, tag);

        // No qty column (review item 14: whole-reel picks, count is noise).
        row_add_main_two_line(row,
                              it.part_name[0] ? it.part_name : it.part_id,
                              it.part_id);

        // Status goes green when the reel is pulled (review item 13).
        if (it.picked)
            row_add_status(row, "Done", theme::color::slot_picked());
        else if (it.slot_num > 0)
            row_add_status(row, "Pending", theme::color::text_muted());
        else
            row_add_status(row, "Not in rack", theme::color::slot_error());
    }

    // Bottom action bar. Cancel is offered only while the job is still in
    // progress; once every line is picked the reels are physically gone and
    // there's nothing to cancel, so only "Complete job" remains (review
    // item 15). Cancel = "abandon the job, leave reels in place" (un-targets
    // the slots, no inventory change). Complete = "finish, free picked slots".
    lv_obj_t* actions = lv_obj_create(body);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(actions, 8, 0);
    lv_obj_set_style_pad_gap(actions, 8, 0);
    lv_obj_set_style_bg_color(actions, color::surface(), 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(actions, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(actions, color::border(), 0);
    lv_obj_set_style_border_width(actions, 1, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END,
                                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    if (j.n_items > 0 && done >= j.n_items) {
        button(actions, "Complete job", BtnKind::Success, complete_active_job);
    } else {
        button(actions, "Cancel job", BtnKind::Danger, cancel_active_job);
    }
}

} // namespace ui::screens
