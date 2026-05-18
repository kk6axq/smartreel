#include "net/wifi_mgr.h"
#include "storage/config_store.h"
#include "ui/app_state.h"
#include "ui/status_bar.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp32-hal-log.h>

namespace wifi_mgr {

static State        g_state = State::Disabled;
static char         g_ip[20]  = "";
static int32_t      g_rssi    = 0;

static void on_event(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
            g_state = State::Connected;
            snprintf(g_ip, sizeof(g_ip), "%s", WiFi.localIP().toString().c_str());
            g_rssi  = WiFi.RSSI();
            app::state().online = true;
            ui::status_bar_set_online(true);
            log_i("WiFi connected: %s @ %d dBm", g_ip, (int)g_rssi);
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (g_state == State::Connected) {
                log_w("WiFi link dropped");
            }
            g_state = State::Disconnected;
            g_ip[0] = 0;
            g_rssi  = 0;
            app::state().online = false;
            ui::status_bar_set_online(false);
            // Arduino-ESP32's WiFi.begin auto-reconnects by default, so
            // we don't trigger anything here. Status will go back to
            // Connected when the AP comes back.
            break;

        case ARDUINO_EVENT_WIFI_STA_START:
            g_state = State::Connecting;
            break;

        default:
            break;
    }
}

void init() {
    auto& w = config_store::cfg().wifi;

    if (!w.enabled) {
        g_state = State::Disabled;
        log_i("WiFi: disabled in config");
        app::state().online = false;
        return;
    }
    if (w.ssid[0] == 0) {
        g_state = State::NoCredentials;
        log_w("WiFi: enabled but no SSID set");
        app::state().online = false;
        return;
    }

    WiFi.onEvent(on_event);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);   // we own credentials in config_store
    WiFi.begin(w.ssid, w.password);
    g_state = State::Connecting;
    log_i("WiFi: connecting to '%s'", w.ssid);
}

void apply() {
    // Tear down any existing association then try again with whatever
    // is currently in config_store.
    WiFi.disconnect(true, true);
    delay(50);
    init();
}

State       status()      { return g_state; }
const char* ipv4()        { return g_ip; }
int32_t     rssi()        {
    if (g_state == State::Connected) g_rssi = WiFi.RSSI();
    return g_rssi;
}

const char* status_str() {
    switch (g_state) {
        case State::Disabled:       return "disabled";
        case State::NoCredentials:  return "no credentials";
        case State::Connecting:     return "connecting";
        case State::Connected:      return "connected";
        case State::Disconnected:   return "reconnecting";
    }
    return "?";
}

// ===================================================================
// Scan
// ===================================================================
static ScanEntry g_results[SCAN_MAX];
static int       g_n_results = 0;

int scan_run() {
    // WiFi.scanNetworks() can collide with an active connection; the
    // Arduino layer handles the pause/resume internally but we still
    // want to start from a known mode.
    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
    }
    int n = WiFi.scanNetworks(/*async*/false, /*show_hidden*/false);
    if (n < 0) n = 0;
    if (n > SCAN_MAX) n = SCAN_MAX;
    g_n_results = n;
    for (int i = 0; i < n; ++i) {
        snprintf(g_results[i].ssid, sizeof(g_results[i].ssid), "%s",
                 WiFi.SSID(i).c_str());
        g_results[i].rssi = WiFi.RSSI(i);
        g_results[i].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    return n;
}

int               scan_count()    { return g_n_results; }
const ScanEntry*  scan_entries()  { return g_results; }

// ===================================================================
// Credential management
// ===================================================================
void set_credentials(const char* ssid, const char* password) {
    auto& w = config_store::cfg().wifi;
    snprintf(w.ssid,     sizeof(w.ssid),     "%s", ssid     ? ssid     : "");
    snprintf(w.password, sizeof(w.password), "%s", password ? password : "");
    w.enabled = (w.ssid[0] != 0);

    // Mirror to app_state so the UI reflects the change immediately.
    config_store::apply_to_app_state();

    // Persist. Best-effort: if no SD, the change still works for this
    // session, just won't survive a reboot.
    if (!config_store::save_config()) {
        log_w("WiFi: could not persist credentials (no SD?)");
    }

    // Reconnect. apply() tears down any existing association first.
    apply();
}

} // namespace wifi_mgr
