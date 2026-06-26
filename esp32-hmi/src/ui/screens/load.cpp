// LOAD -- scan an InvenTree QR, then place:
//   Scan / watching : live QR poll; "Cancel". A recognised scan resolves
//                     against InvenTree and auto-advances to Placed.
//   Placed          : clickable dot grid lights all empty slots, plus
//                     "Rescan" / "Cancel".
//
// There is no offline/simulated load path: a load must resolve to a real
// InvenTree stock item so the placement can be reported back. The QR
// scanner free-runs (it re-reports whatever code is in view), so we latch
// the first recognised scan and stop polling until a rescan.
#include "ui/screens/screens.h"
#include "ui/screen_manager.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "ui/notify.h"
#include "sensors/qr_scanner.h"
#include "net/inv_api.h"
#include "util/lvgl_async.h"
#include "app/leds.h"
#include "app/beeper.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

// Action log to the USB serial console, always on, greppable "[load]"
// prefix — mirrors inv_sync's "[inv]" so the scan->resolve->place flow is
// visible alongside the op layer.
#define LOAD_LOG(fmt, ...) Serial.printf("[load] " fmt "\n", ##__VA_ARGS__)

namespace ui::screens {

using namespace theme;

namespace {
    constexpr int POLL_MS = 200;          // 5 Hz, well within device cap
    lv_timer_t* g_timer       = nullptr;
    lv_obj_t*   g_status_lbl  = nullptr;  // live status, only in watching view
    char        g_status_cache[96] = {};

    // True while an inv_api::resolve_barcode() worker is running. The
    // poll timer no-ops while in flight so a free-running scanner
    // doesn't fire off a queue of identical requests.
    bool g_resolve_in_flight = false;

    // Last QR we kicked off a resolve for. Used to dedupe the case where
    // the scanner re-reports the same code 5 times/sec while we're
    // already working on it.
    char g_last_resolve_qr[64] = {};

    // Scan errors used to vanish the instant the reel moved out of view
    // (the next poll overwrote them with "Watching..."). Hold an error on
    // screen for at least this long so the operator can read it (review
    // item 20). A non-error status is suppressed until the dwell elapses.
    constexpr uint32_t ERROR_DWELL_MS = 2500;
    uint32_t g_error_until_ms = 0;
}

// Set the watching-view status line, skipping the redraw if unchanged.
// warn=true starts an error dwell; a non-warn update is held off until the
// dwell elapses so a transient "Watching..." can't wipe the error early.
static void set_status(const char* text, bool warn) {
    if (!g_status_lbl) return;
    const uint32_t now = millis();
    if (warn) g_error_until_ms = now + ERROR_DWELL_MS;
    else if ((int32_t)(g_error_until_ms - now) > 0) return;   // keep error up
    if (strcmp(g_status_cache, text) == 0) return;
    snprintf(g_status_cache, sizeof(g_status_cache), "%s", text);
    lv_label_set_text(g_status_lbl, text);
    lv_obj_set_style_text_color(g_status_lbl,
                                warn ? color::slot_warn() : color::text_muted(), 0);
}

// Error status + error tone (review items 5, 20).
static void scan_error(const char* text) {
    set_status(text, true);
    beeper::error();
}

// ---- Resolve worker ------------------------------------------------
//
// A QR code goes:
//   load poll detects code -> kick worker task
//   worker calls inv_api::resolve_barcode() (blocking, ~50-500 ms)
//   worker dispatches the result back to the LVGL task
//   LVGL callback either locks the part or surfaces an error
//
// If inv_api isn't usable (no wifi / not configured), the scan can't be
// resolved and the status line says so -- there is no offline load.

struct ResolveReq {
    char qr[64];
    char op_id[40];
};

struct ResolveDone {
    inv_api::ResolveResult result;
    char                   qr[64];
};

// Convert inv_api::Part into the firmware's app::Part struct -- same
// fields, slightly different sizes. snprintf truncates safely.
static void copy_part(app::Part& dst, const inv_api::Part& src) {
    snprintf(dst.id,   sizeof(dst.id),   "%s", src.id);
    snprintf(dst.name, sizeof(dst.name), "%s", src.name);
    snprintf(dst.pkg,  sizeof(dst.pkg),  "%s", src.pkg);
    snprintf(dst.mfg,  sizeof(dst.mfg),  "%s", src.mfg);
    dst.valid = true;
}

// Once a scan locks a part we skip the separate "confirm + Place"
// card and go straight into placement: light the open slots and show
// the dot grid. The scanned part is still shown (the placed view keeps
// the scan card up top), and Rescan/Cancel there recover a misscan.
static void advance_to_placement() {
    app::load_begin_placement();
    leds::light_target_slots();
    ui::rebuild_current();
}

// Runs on the LVGL task after the worker completes. Always frees its
// argument, always clears the in-flight flag.
static void on_resolve_done(void* user) {
    auto* d = static_cast<ResolveDone*>(user);
    g_resolve_in_flight = false;

    // If the user has navigated away or already locked a scan, drop
    // the result on the floor. (Race: scanner fires, user taps
    // Simulate scan or Cancel before the network call returns.)
    if (ui::current() != ui::Screen::Load ||
        app::state().load_step != app::LoadStep::Scan ||
        app::state().load_scan_locked) {
        delete d;
        return;
    }

    const auto& r = d->result;
    switch (r.status) {
        case inv_api::Status::Ok: {
            if (r.type == inv_api::ResolveType::StockItem) {
                LOAD_LOG("resolved stockitem id=%d qty=%d slot_num=%d part=%s",
                         r.stock.id, r.stock.qty, r.stock.slot_num, r.stock.part.id);
                // Already housed in a rack slot? Tell the user instead
                // of starting a duplicate load (user story 1 / contract).
                // Already housed in a slot of THIS rack -> reject (can't load
                // the same reel twice here). A reel in a *different* SmartReel
                // is accepted: loading transfers it over, and that rack notices
                // the move via its fast occupancy poll (review items 6, 21).
                if (r.stock.slot_num > 0) {
                    LOAD_LOG("  already in this rack slot %d -- not loading",
                             r.stock.slot_num);
                    char msg[96];
                    snprintf(msg, sizeof(msg), "%s is already in slot %d",
                             r.stock.part.id, r.stock.slot_num);
                    scan_error(msg);
                    break;
                }
                app::Part p; copy_part(p, r.stock.part);
                app::load_apply_part(p, r.stock.id, r.stock.qty);
                advance_to_placement();
                delete d; return;
            }
            if (r.type == inv_api::ResolveType::ItemPart) {
                // A part QR names the part, not a specific reel. Loading
                // needs a stock item to transfer into the slot, so refuse
                // and tell the user to scan the reel's own (StockItem) QR.
                LOAD_LOG("resolved PART %s -- not a stock item, refusing load", r.part.id);
                char msg[120];
                snprintf(msg, sizeof(msg),
                         "%s is a part code, not a reel. Scan the reel's "
                         "stock-item QR instead.", r.part.id);
                scan_error(msg);
                break;
            }
            if (r.type == inv_api::ResolveType::Location) {
                LOAD_LOG("resolved LOCATION -- not a stock item, refusing load");
                scan_error("That's a location code, not a reel. Scan the "
                           "reel's stock-item QR.");
                break;
            }
            // type=unknown -- server rejected the code.
            LOAD_LOG("resolve: unknown code '%.40s'", d->qr);
            char msg[96];
            snprintf(msg, sizeof(msg), "Unrecognised code: %.40s", d->qr);
            scan_error(msg);
            // Allow the same code to be tried again only after the
            // scanner has reported a different one in the meantime.
            break;
        }
        case inv_api::Status::NotConfigured:
            LOAD_LOG("resolve: InvenTree not configured");
            set_status("InvenTree not configured. Settings > Network to set it up.", true);
            break;
        case inv_api::Status::NoWifi:
            LOAD_LOG("resolve: no connection");
            set_status("Not connected to InvenTree. Loading needs a connection.", true);
            break;
        default: {
            LOAD_LOG("resolve FAILED: %s (http %d)",
                     r.error[0] ? r.error : inv_api::status_str(r.status), r.http_code);
            char msg[96];
            snprintf(msg, sizeof(msg), "Resolve failed: %.60s",
                     r.error[0] ? r.error : inv_api::status_str(r.status));
            scan_error(msg);
            // Clear g_last_resolve_qr so a retry of the same code does
            // re-fire after a transient failure.
            g_last_resolve_qr[0] = 0;
            break;
        }
    }
    delete d;
}

static void resolve_worker(void* arg) {
    auto* req = static_cast<ResolveReq*>(arg);
    inv_api::ResolveResult res =
        inv_api::resolve_barcode(req->qr, req->op_id);

    auto* done = new ResolveDone();
    done->result = res;
    snprintf(done->qr, sizeof(done->qr), "%s", req->qr);
    delete req;

    ui::dispatch_on_lvgl(on_resolve_done, done);
    vTaskDelete(nullptr);
}

static void start_resolve(const char* qr) {
    g_resolve_in_flight = true;
    LOAD_LOG("scan '%.48s' -> resolving against InvenTree", qr);
    snprintf(g_last_resolve_qr, sizeof(g_last_resolve_qr), "%s", qr);

    auto* req = new ResolveReq();
    snprintf(req->qr, sizeof(req->qr), "%s", qr);
    inv_api::make_op_id(req->op_id, sizeof(req->op_id));

    // Pin to APP_CPU at priority 1 (LVGL is also on APP_CPU at
    // priority 2 -- it preempts us). PRO_CPU services the RGB LCD
    // DMA refresh; running TLS + JSON there during a scan causes
    // visible tearing (see comment in main.cpp:lvgl_task creation).
    // 6 KB stack: HTTPClient + NetworkClientSecure + ArduinoJson fit
    // comfortably; same sizing as the network-screen test worker.
    BaseType_t ok = xTaskCreatePinnedToCore(
        resolve_worker, "inv-resolve", 6 * 1024, req, 1, nullptr, APP_CPU_NUM);
    if (ok != pdPASS) {
        delete req;
        g_resolve_in_flight = false;
        set_status("Resolve task failed to start.", true);
    }
}

// 5 Hz poll. No-ops unless the user is actively watching for a scan on
// the Load screen (current screen, Scan step, not yet locked, no
// resolve in flight).
static void on_poll(lv_timer_t*) {
    if (ui::current() != ui::Screen::Load) return;
    if (app::state().load_step != app::LoadStep::Scan) return;
    if (app::state().load_scan_locked) return;
    if (g_resolve_in_flight) return;     // wait for the worker to finish

    if (!qr_scanner::present()) {
        set_status("No QR scanner detected on I2C (0x0C).", true);
        return;
    }

    char buf[256];
    if (qr_scanner::poll(buf, sizeof(buf))) {
        // The scanner free-runs (re-reports the same code 5x/sec while
        // it's in view); skip if we already resolved this exact code
        // and it's still on the status line.
        if (g_last_resolve_qr[0] && strcmp(g_last_resolve_qr, buf) == 0) {
            return;
        }
        set_status("Resolving...", false);
        start_resolve(buf);
        return;
    }
    // No code this tick -- the user may have moved the reel away after
    // a failed resolve, so clear the dedupe key so the next sighting
    // (possibly the same code) re-fires.
    g_last_resolve_qr[0] = 0;
    set_status("Watching... point the reel's QR code at the scanner.", false);
}

static void ensure_timer() {
    if (!g_timer) g_timer = lv_timer_create(on_poll, POLL_MS, nullptr);
}

// ---- Handlers ------------------------------------------------------
// Rescan from the placement view: revert the lit targets, drop back to
// watching for a fresh code. mock_cancel_load() resets load_step to Scan
// and clears the locked part; we stay on the Load screen.
static void on_rescan(lv_event_t*) {
    char drain[256];
    (void)qr_scanner::poll(drain, sizeof(drain));   // flush the latched code
    g_last_resolve_qr[0] = 0;        // forget the previous resolve dedupe
    app::mock_cancel_load();
    leds::clear_all();               // drop the lit placement targets
    ui::rebuild_current();
}
static void on_cancel(lv_event_t*) {
    app::mock_cancel_load();
    leds::clear_all();               // drop any lit placement targets
    ui::go_back();
}
// One-shot: clear a slot's confirmation LED a couple seconds after a load
// (review item 4). Allocated per use; freed here.
struct GreenClearCtx { int slot; };
static void green_clear_cb(lv_timer_t* t) {
    auto* c = static_cast<GreenClearCtx*>(t->user_data);
    if (c) { leds::light_slot(c->slot, 0, 0, 0); delete c; }
    lv_timer_del(t);
}
// Confirm a successful load: flash the slot green for 2 s, tone, and toast
// (review item 4). Shared by the on-screen dot pick and the hardware
// (reel-inserted) placement path in main.cpp.
void load_confirm_placed(int slot_num) {
    leds::clear_all();                                   // drop the blue targets
    leds::light_slot(slot_num, 0x16, 0xA3, 0x4A);        // theme green
    auto* c = new GreenClearCtx{ slot_num };
    lv_timer_t* t = lv_timer_create(green_clear_cb, 2000, c);
    lv_timer_set_repeat_count(t, 1);
    beeper::ok();
    char msg[40];
    snprintf(msg, sizeof(msg), "Loaded to slot #%d", slot_num);
    ui::toast(msg, ui::NotifyMood::Success);
}

static void on_dot_pick(int slot_num) {
    LOAD_LOG("place (on-screen dot) slot=%d stock_id=%d",
             slot_num, app::state().load_stock_id);
    app::mock_place_reel(slot_num);  // logs '[inv] queue assign' when stock_id>0
    load_confirm_placed(slot_num);   // green flash + tone + toast (item 4)
    ui::navigate(Screen::Home);
}

// ---- Scan: watching view ------------------------------------------
static void build_watching_state(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_style_pad_gap(body, 14, 0);

    // Big left-pointing "<-" arrow toward the physical scanner (it sits on the
    // left of the unit, review item R2). Drawn from line primitives rather than
    // a font glyph so it can be made arbitrarily large with a true arrow shape.
    // Anchored to the left edge, centred vertically, and excluded from the
    // body's flex layout so it overlays independently of the scan prompt.
    {
        // LVGL keeps the point array by pointer (it doesn't copy), so these
        // must outlive the widget -- hence static. Container is 180x180; the
        // arrow is scaled to ~70% length (about the left tip + vertical centre)
        // so its shaft no longer reaches the centred scan text.
        static const lv_point_t head_pts[]  = { {76, 37}, {12, 90}, {76, 143} };
        static const lv_point_t shaft_pts[] = { {12, 90}, {166, 90} };

        lv_obj_t* arrow = lv_obj_create(body);
        lv_obj_remove_style_all(arrow);
        lv_obj_set_size(arrow, 180, 180);
        lv_obj_clear_flag(arrow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(arrow, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(arrow, LV_ALIGN_LEFT_MID, 0, 0);

        auto add_stroke = [&](const lv_point_t* pts, uint16_t n) {
            lv_obj_t* line = lv_line_create(arrow);
            lv_line_set_points(line, pts, n);
            lv_obj_set_style_line_width(line, 18, 0);
            lv_obj_set_style_line_color(line, color::accent(), 0);
            lv_obj_set_style_line_rounded(line, true, 0);
        };
        add_stroke(head_pts, 3);   // the "<" arrowhead
        add_stroke(shaft_pts, 2);  // the "-" shaft
    }

    // Big QR placeholder box
    lv_obj_t* qr = lv_obj_create(body);
    lv_obj_remove_style_all(qr);
    lv_obj_set_size(qr, 96, 96);
    lv_obj_set_style_border_color(qr, color::text(), 0);
    lv_obj_set_style_border_width(qr, 4, 0);
    lv_obj_set_style_radius(qr, layout::RADIUS, 0);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* qr_l = lv_label_create(qr);
    lv_label_set_text(qr_l, "QR");
    lv_obj_set_style_text_font(qr_l, &lv_font_montserrat_28, 0);
    lv_obj_center(qr_l);

    lv_obj_t* prompt = lv_label_create(body);
    lv_label_set_text(prompt, "Scan reel barcode");
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(prompt, color::text(), 0);

    // Live status line, driven by the poll timer.
    g_status_lbl = lv_label_create(body);
    lv_obj_set_style_text_font(g_status_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_status_lbl, color::text_muted(), 0);
    lv_obj_set_style_text_align(g_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_status_lbl, 480);
    lv_label_set_text(g_status_lbl,
                      "Point the scanner at the reel's InvenTree QR code.");
    g_status_cache[0] = 0;   // force the first timer paint

    lv_obj_t* btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    button(btn_row, "Cancel", BtnKind::Default, on_cancel);
}

// ---- Placed view (slot grid) --------------------------------------
static void build_placed_state(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_gap(body, 10, 0);

    auto& part = app::state().load_part;
    int lit = app::slots_target();

    // Primary call-to-action, big and at the very top (review item 19).
    lv_obj_t* head = lv_label_create(body);
    lv_label_set_text(head, "Place reel in any lit slot");
    lv_obj_set_style_text_font(head, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(head, color::text(), 0);
    lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(head, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(head, LV_PCT(100));

    char sub[48];
    snprintf(sub, sizeof(sub), "%d slot%s available", lit, lit == 1 ? "" : "s");
    lv_obj_t* subl = lv_label_create(body);
    lv_label_set_text(subl, sub);
    lv_obj_set_style_text_font(subl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(subl, color::text_muted(), 0);
    lv_obj_set_style_text_align(subl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(subl, LV_PCT(100));

    // Scan-result card (what was scanned -- secondary to the heading above)
    lv_obj_t* res = card(body);
    lv_obj_set_width(res, LV_PCT(100));

    lv_obj_t* nm = lv_label_create(res);
    lv_label_set_text(nm, part.name);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(nm, color::text(), 0);

    char meta[96];
    if (app::state().load_qty > 0)
        snprintf(meta, sizeof(meta), "%s  %s  %s  -  qty %d",
                 part.id, part.pkg, part.mfg, app::state().load_qty);
    else
        snprintf(meta, sizeof(meta), "%s  %s  %s", part.id, part.pkg, part.mfg);
    lv_obj_t* mt = lv_label_create(res);
    lv_label_set_text(mt, meta);
    lv_obj_set_style_text_font(mt, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(mt, color::text_muted(), 0);

    // Clickable dot grid (only TARGET dots respond)
    DotGridOpts dg = {};
    dg.show_legend  = false;
    dg.header_label = "Available slots";
    dg.clickable    = true;
    dg.on_dot_click = on_dot_pick;
    dot_grid_card(body, dg);

    lv_obj_t* btn_row = lv_obj_create(body);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                                    LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    // Place is now automatic on scan; Rescan recovers a misscan without
    // leaving the Load screen, Cancel backs all the way out.
    button(btn_row, "Rescan", BtnKind::Default, on_rescan);
    button(btn_row, "Cancel", BtnKind::Default, on_cancel);
}

void build_load(lv_obj_t* body) {
    // g_status_lbl belongs to the watching view only; clear it so the
    // poll timer never touches a stale pointer after a rebuild.
    g_status_lbl = nullptr;

    // A recognised scan locks the part and immediately advances to
    // LoadStep::Placed (see advance_to_placement), so there's no separate
    // "locked, awaiting Place" view: watch while scanning, grid once placed.
    if (app::state().load_step == app::LoadStep::Placed) build_placed_state(body);
    else                                                 build_watching_state(body);
    ensure_timer();
}

} // namespace ui::screens
