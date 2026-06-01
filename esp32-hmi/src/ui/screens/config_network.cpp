// CONFIG-NETWORK -- live WiFi status + scan list + Inventree config.
//
// Tap a row in the scan list to open the password modal; on Connect
// the credentials are saved and the radio re-associates. The screen
// is rebuilt on entry, so live status reflects the latest event from
// wifi_mgr.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/screen_manager.h"
#include "ui/wifi_password_modal.h"
#include "ui/text_entry_modal.h"
#include "net/wifi_mgr.h"
#include "net/inv_api.h"
#include "util/lvgl_async.h"
#include "ui/app_state.h"
#include "storage/config_store.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

namespace ui::screens {

using namespace theme;

// ---- Status helpers -------------------------------------------------
static lv_color_t status_color(wifi_mgr::State s) {
    switch (s) {
        case wifi_mgr::State::Connected:    return color::slot_picked();
        case wifi_mgr::State::Connecting:   return color::accent();
        case wifi_mgr::State::Disconnected: return color::slot_warn();
        default:                            return color::text_muted();
    }
}

// Translate an RSSI dBm reading into a 4-bar string, JetBrains-mono style.
static const char* signal_bars(int rssi) {
    if (rssi >= -55) return "▮▮▮▮";
    if (rssi >= -65) return "▮▮▮ ";
    if (rssi >= -75) return "▮▮  ";
    if (rssi >= -85) return "▮   ";
    return "    ";
}

// ---- Scan-row tap → open password modal ----------------------------
struct RowCtx { char ssid[33]; bool open; };

static void on_row_clicked(lv_event_t* e) {
    auto* ctx = static_cast<RowCtx*>(lv_event_get_user_data(e));
    if (!ctx) return;
    if (ctx->open) {
        // Open networks: skip password entry, connect directly.
        wifi_mgr::set_credentials(ctx->ssid, "");
        ui::rebuild_current();
    } else {
        ui::wifi_password_modal_open(ctx->ssid, false);
    }
}
static void on_row_ctx_free(lv_event_t* e) {
    delete static_cast<RowCtx*>(lv_event_get_user_data(e));
}

// ---- Action handlers -----------------------------------------------
static void on_scan(lv_event_t*) {
    wifi_mgr::scan_run();
    ui::rebuild_current();
}

static void on_disconnect(lv_event_t*) {
    wifi_mgr::set_credentials("", "");   // empty SSID disables WiFi
    ui::rebuild_current();
}

static void on_manual_entry(lv_event_t*) {
    // Stash the special "manual" SSID; the modal accepts free text but
    // for now we just open it pre-filled with empty SSID. The user
    // can correct after typing — actual hidden-network entry needs a
    // second input. Simple form: just reuse the modal as a password
    // editor for the *current* configured SSID if any, otherwise tell
    // the user to scan first.
    auto& w = config_store::cfg().wifi;
    if (w.ssid[0]) {
        ui::wifi_password_modal_open(w.ssid, false);
    } else {
        // No-op. We rely on the user scanning first; a hidden-network
        // editor is a future enhancement.
    }
}

// ---- Main builder ---------------------------------------------------
static void build_status_card(lv_obj_t* parent) {
    lv_obj_t* fc = form_card(parent, "WI-FI STATUS");

    auto state = wifi_mgr::status();

    // Status pill row
    lv_obj_t* r = form_row(fc);
    form_row_label(r, "State", nullptr);
    lv_obj_t* st = lv_label_create(r);
    lv_label_set_text(st, wifi_mgr::status_str());
    lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st, status_color(state), 0);

    // Network name
    auto& w = config_store::cfg().wifi;
    r = form_row(fc);
    form_row_label(r, "Network", nullptr);
    lv_obj_t* sl = lv_label_create(r);
    lv_label_set_text(sl, w.ssid[0] ? w.ssid : "(none configured)");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sl, w.ssid[0] ? color::text() : color::text_muted(), 0);

    // IP + RSSI when connected
    if (state == wifi_mgr::State::Connected) {
        r = form_row(fc);
        form_row_label(r, "IP address", nullptr);
        lv_obj_t* ip = lv_label_create(r);
        lv_label_set_text(ip, wifi_mgr::ipv4());
        lv_obj_set_style_text_font(ip, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ip, color::text(), 0);

        char rb[32];
        snprintf(rb, sizeof(rb), "%d dBm", (int)wifi_mgr::rssi());
        r = form_row(fc);
        form_row_label(r, "Signal", nullptr);
        lv_obj_t* rl = lv_label_create(r);
        lv_label_set_text(rl, rb);
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rl, color::text(), 0);
    }

    // Action row: Disconnect (only when configured)
    if (w.ssid[0]) {
        r = form_row(fc);
        // empty left label column
        lv_obj_t* spacer = lv_obj_create(r);
        lv_obj_remove_style_all(spacer);
        lv_obj_set_flex_grow(spacer, 1);
        lv_obj_set_height(spacer, 1);
        lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
        button(r, "Edit password", BtnKind::Default, on_manual_entry);
        button(r, "Disconnect",     BtnKind::Danger,  on_disconnect);
    }
}

static void build_scan_card(lv_obj_t* parent) {
    lv_obj_t* fc = form_card(parent, "AVAILABLE NETWORKS");

    int n = wifi_mgr::scan_count();
    const wifi_mgr::ScanEntry* results = wifi_mgr::scan_entries();

    // Header row with scan button
    lv_obj_t* r = form_row(fc);
    char hdr[40];
    snprintf(hdr, sizeof(hdr), "%d network%s found", n, n == 1 ? "" : "s");
    form_row_label(r, hdr, "Tap a row to connect");
    button(r, "Scan", BtnKind::Primary, on_scan);

    if (n == 0) {
        r = form_row(fc);
        lv_obj_t* l = lv_label_create(r);
        lv_label_set_text(l, "No scan results. Tap Scan to search.");
        lv_obj_set_style_text_color(l, color::text_muted(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }

    for (int i = 0; i < n; ++i) {
        const auto& e = results[i];
        lv_obj_t* row = form_row(fc);
        // Make the entire row clickable.
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        auto* ctx = new RowCtx{};
        snprintf(ctx->ssid, sizeof(ctx->ssid), "%s", e.ssid);
        ctx->open = e.open;
        lv_obj_add_event_cb(row, on_row_clicked,  LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, on_row_ctx_free, LV_EVENT_DELETE,  ctx);

        char meta[40];
        snprintf(meta, sizeof(meta), "%s  %d dBm  %s",
                 signal_bars(e.rssi), e.rssi,
                 e.open ? "OPEN" : "PSK");
        form_row_label(row, e.ssid[0] ? e.ssid : "(hidden)", meta);

        lv_obj_t* arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arrow, color::text_muted(), 0);
    }
}

// ---- Inventree config + connection test ---------------------------
//
// URL and API token are edited via the shared text_entry_modal; the
// rack location ID via a simple +/- nudge stored as an int. Save runs
// every keystroke commit through config_store::save_config() so the
// change survives a reboot.
//
// "Test connection" runs inv_api::health() on a one-shot worker task
// (HTTPClient is blocking, can't sit on the LVGL thread). When it
// returns, the result is posted back via lv_async_call and we rebuild
// the screen. Test-in-progress is a tiny flag; the button stays
// labelled "Testing..." until the worker completes.

namespace {
    bool g_test_in_flight = false;
}

static void persist_config() {
    if (!config_store::save_config()) {
        Serial.println("[net-cfg] save_config failed (no SD?)");
    }
}

// ---- text edit callbacks ------------------------------------------

static void on_url_saved(const char* v) {
    auto& iv = config_store::cfg().inventree;
    snprintf(iv.url, sizeof(iv.url), "%s", v ? v : "");
    persist_config();
    ui::rebuild_current();
}
static void on_token_saved(const char* v) {
    auto& iv = config_store::cfg().inventree;
    snprintf(iv.token, sizeof(iv.token), "%s", v ? v : "");
    persist_config();
    ui::rebuild_current();
}
static void on_locid_saved(const char* v) {
    if (!v) return;
    int n = atoi(v);
    if (n < 0) n = 0;
    config_store::cfg().inventree.location_id = n;
    persist_config();
    ui::rebuild_current();
}

static void on_url_tap(lv_event_t*) {
    ui::TextEntryOpts o = {};
    o.title       = "InvenTree URL";
    o.initial     = config_store::cfg().inventree.url;
    o.placeholder = "https://192.168.1.10:8443";
    o.max_len     = sizeof(config_store::cfg().inventree.url) - 1;
    o.on_save     = on_url_saved;
    ui::text_entry_modal_open(o);
}
static void on_token_tap(lv_event_t*) {
    ui::TextEntryOpts o = {};
    o.title         = "API token";
    o.initial       = config_store::cfg().inventree.token;
    o.placeholder   = "dev-token";
    o.password_mode = true;
    o.max_len       = sizeof(config_store::cfg().inventree.token) - 1;
    o.on_save       = on_token_saved;
    ui::text_entry_modal_open(o);
}
static void on_locid_tap(lv_event_t*) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", config_store::cfg().inventree.location_id);
    ui::TextEntryOpts o = {};
    o.title       = "Rack location ID";
    o.initial     = buf;
    o.placeholder = "42";
    o.max_len     = 8;
    o.on_save     = on_locid_saved;
    ui::text_entry_modal_open(o);
}

// ---- connection test -----------------------------------------------
//
// Worker runs blocking inv_api::health(), then hops back to LVGL via
// dispatch_on_lvgl() to rebuild the screen so the new status shows up.
// The result itself is kept inside inv_api (last_health()).

static void test_done_on_lvgl(void*) {
    g_test_in_flight = false;
    // Only redraw if the user is still on this screen; otherwise leave
    // them where they navigated to.
    if (ui::current() == ui::Screen::ConfigNetwork) ui::rebuild_current();
}

static void test_worker(void* /*arg*/) {
    (void)inv_api::health();    // result is cached inside inv_api
    ui::dispatch_on_lvgl(test_done_on_lvgl, nullptr);
    vTaskDelete(nullptr);
}

static void on_test(lv_event_t*) {
    if (g_test_in_flight) return;
    g_test_in_flight = true;
    ui::rebuild_current();
    // Pin to APP_CPU at priority 1: PRO_CPU services the RGB LCD
    // refresh DMA and must not be loaded with TLS / JSON work or the
    // framebuffer flush tears. LVGL is also on APP_CPU at priority 2
    // so it preempts us. 6 KB stack: NetworkClientSecure + ArduinoJson
    // + HTTPClient fit comfortably (~3.5 KB peak measured).
    xTaskCreatePinnedToCore(test_worker, "inv-health", 6 * 1024,
                            nullptr, 1, nullptr, APP_CPU_NUM);
}

// ---- "last seen" line ----------------------------------------------

static const char* health_label() {
    const auto& h = inv_api::last_health();
    static char buf[96];
    if (g_test_in_flight) return "Testing...";
    switch (h.status) {
        case inv_api::Status::Ok: {
            uint32_t age_s = (millis() - inv_api::last_success_ms()) / 1000;
            snprintf(buf, sizeof(buf), "Online: %s v%s (%lus ago)",
                     h.server[0] ? h.server : "?",
                     h.version[0] ? h.version : "?",
                     (unsigned long)age_s);
            return buf;
        }
        case inv_api::Status::NotConfigured:
            return "Not configured -- enter URL + token, then Test.";
        case inv_api::Status::NoWifi:
            return "No Wi-Fi connection.";
        default:
            snprintf(buf, sizeof(buf), "Last test failed: %s",
                     h.error[0] ? h.error : inv_api::status_str(h.status));
            return buf;
    }
}

static lv_color_t health_color() {
    const auto& h = inv_api::last_health();
    if (g_test_in_flight) return color::accent();
    if (h.status == inv_api::Status::Ok) return color::slot_picked();
    if (h.status == inv_api::Status::NotConfigured) return color::text_muted();
    return color::slot_warn();
}

// ---- builder -------------------------------------------------------

// Make a form_input row "tappable": wraps the input box in a clickable
// area that opens the text-entry modal. Returns the row for further
// composition. The input box's own click handler isn't enough because
// it lives inside the row; we attach to the row instead.
static lv_obj_t* tappable_field_row(lv_obj_t* card, const char* label,
                                    const char* desc, const char* value,
                                    int width, bool masked,
                                    lv_event_cb_t handler) {
    lv_obj_t* r = form_row(card);
    form_row_label(r, label, desc);
    form_input(r, masked && value && value[0] ? "************" : value,
               width, false);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, handler, LV_EVENT_CLICKED, nullptr);
    return r;
}

static void build_inventree_card(lv_obj_t* parent) {
    lv_obj_t* fc = form_card(parent, "INVENTREE SERVER");

    auto& iv = config_store::cfg().inventree;

    tappable_field_row(fc, "URL", "Tap to edit",
                       iv.url[0] ? iv.url : "(not set)", 280, false,
                       on_url_tap);

    tappable_field_row(fc, "API token", "Tap to edit",
                       iv.token, 200, true,
                       on_token_tap);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", iv.location_id);
    tappable_field_row(fc, "Rack location ID", "Inventree stock location",
                       buf, 0, false, on_locid_tap);

    // ---- Test connection + status strip ----
    lv_obj_t* r = form_row(fc);
    lv_obj_t* lbl = lv_label_create(r);
    lv_label_set_text(lbl, health_label());
    lv_obj_set_style_text_color(lbl, health_color(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lbl, 1);

    button(r, g_test_in_flight ? "Testing..." : "Test",
           BtnKind::Primary, on_test);
}

void build_config_network(lv_obj_t* body) {
    lv_obj_t* sc = form_scroller(body);
    build_status_card(sc);
    build_scan_card(sc);
    build_inventree_card(sc);
}

} // namespace ui::screens
