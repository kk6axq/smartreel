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
#include "net/wifi_mgr.h"
#include "ui/app_state.h"
#include "storage/config_store.h"

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

static void build_inventree_card(lv_obj_t* parent) {
    lv_obj_t* fc = form_card(parent, "INVENTREE SERVER");

    lv_obj_t* r = form_row(fc);
    form_row_label(r, "URL", nullptr);
    form_input(r, config_store::cfg().inventree.url, 280);

    r = form_row(fc);
    form_row_label(r, "API token", "Tap to reveal & edit");
    form_input(r, "************", 200);

    r = form_row(fc);
    form_row_label(r, "Rack location ID", "Inventree stock location");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", config_store::cfg().inventree.location_id);
    form_input(r, buf, 0, true);
}

void build_config_network(lv_obj_t* body) {
    lv_obj_t* sc = form_scroller(body);
    build_status_card(sc);
    build_scan_card(sc);
    build_inventree_card(sc);
}

} // namespace ui::screens
