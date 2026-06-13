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
#include "storage/state_store.h"
#include "app/leds.h"
#include "app/inv_sync.h"
#include "net/inv_api.h"

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
// dispatch and rebuilds this screen with the live list.
static void on_jobs_refresh(lv_event_t*) {
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
struct PickedCtx { int item_idx; };

static void on_picked(lv_event_t* e) {
    auto* c = static_cast<PickedCtx*>(lv_event_get_user_data(e));
    if (!c) return;
    auto& st = app::state();
    if (st.active_pick_idx < 0) return;
    auto& j = st.pick_jobs[st.active_pick_idx];
    if (c->item_idx < 0 || c->item_idx >= j.n_items) return;
    const int slot_num = j.items[c->item_idx].slot_num;
    app::lock();
    j.items[c->item_idx].picked = true;
    app::unlock();
    state_store::mark_jobs_dirty();
    // Manual confirm path (no slot sensor fired). Report the same
    // whole-reel transfer the hardware path would; mirror the local
    // slot bookkeeping too so the rack doesn't think the reel stayed.
    if (slot_num > 0) {
        app::Slot* s = app::slot_by_num(slot_num);
        if (s) {
            app::lock();
            s->state      = app::SlotState::EMPTY;   // reel is gone -> staging
            s->part.valid = false;
            s->qty        = 0;
            app::unlock();
            state_store::mark_slot_dirty(slot_num);
        }
        leds::light_slot(slot_num, 0, 0, 0);   // reel out: darken its LED
        inv_sync::queue_job_pick(j.id, c->item_idx, slot_num);
    }
    ui::rebuild_current();
}
static void free_picked_ctx(lv_event_t* e) {
    delete static_cast<PickedCtx*>(lv_event_get_user_data(e));
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

    // List of items
    lv_obj_t* sc = row_scroller(body);
    lv_obj_set_flex_grow(sc, 1);
    for (int i = 0; i < j.n_items; ++i) {
        auto& it = j.items[i];

        RowOpts ro;
        if (it.picked)            ro.stripe_state = app::SlotState::PICKED;
        else if (it.slot_num > 0) ro.stripe_state = app::SlotState::TARGET;
        else                      ro.stripe_state = app::SlotState::ERROR;

        lv_obj_t* row = row_make(sc, ro);

        char tag[12];
        if (it.slot_num > 0) snprintf(tag, sizeof(tag), "#%d", it.slot_num);
        else                 snprintf(tag, sizeof(tag), "-");
        row_add_slot_num(row, tag);

        char meta[64];
        snprintf(meta, sizeof(meta), "%s  need %d", it.part_id, it.qty);
        row_add_main_two_line(row,
                              it.part_name[0] ? it.part_name : it.part_id,
                              meta);

        char qb[12]; snprintf(qb, sizeof(qb), "%d", it.qty);
        row_add_qty_two_line(row, qb, "qty");

        auto* ctx = new PickedCtx{ i };
        lv_obj_t* b = row_add_button(row, it.picked ? "Done" : "Picked",
                                      it.picked ? nullptr : on_picked,
                                      ctx,
                                      /*muted=*/false,
                                      /*success=*/it.picked,
                                      /*disabled=*/it.picked);
        lv_obj_add_event_cb(b, free_picked_ctx, LV_EVENT_DELETE, ctx);
    }

    // Bottom action bar: Cancel always; Complete once every item is
    // picked (each pick was already reported to InvenTree, so Complete is
    // local cleanup that frees the picked slots and returns to the list).
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

    button(actions, "Cancel job", BtnKind::Danger, cancel_active_job);
    if (j.n_items > 0 && done >= j.n_items) {
        button(actions, "Complete job", BtnKind::Success, complete_active_job);
    }
}

} // namespace ui::screens
