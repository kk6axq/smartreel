// =====================================================================
//  WiFi connection manager.
//
//  Reads credentials from config_store::cfg().wifi and tries to keep
//  the radio associated. Updates app::state().online + the persistent
//  status bar whenever the link state changes.
//
//  Non-blocking: init() kicks off a connection attempt and returns
//  immediately; status() lets the rest of the system poll.
// =====================================================================
#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace wifi_mgr {

enum class State {
    Disabled,        // wifi.enabled == false in config
    NoCredentials,   // SSID is empty
    Connecting,
    Connected,
    Disconnected,    // was connected, link dropped; auto-reconnecting
};

void  init();        // call once, after config_store::init()
State status();
const char* status_str();

// Current IP / RSSI when connected; otherwise zero-initialised.
const char* ipv4();
int32_t      rssi();

// Force a reconnect attempt with the current credentials. Use after
// editing wifi.ssid / wifi.password in config_store.
void apply();

// ----- Scan API -----------------------------------------------------
// Synchronous scan for nearby APs (~3 s). Returns the count, capped
// at SCAN_MAX. Any in-progress association is paused for the scan
// and resumed afterwards. Results stay valid until the next call.
struct ScanEntry {
    char ssid[33];   // 32-byte SSID + NUL
    int  rssi;       // dBm (negative)
    bool open;       // true if no password required
};
constexpr int SCAN_MAX = 16;

int               scan_run();          // returns N entries (>=0)
int               scan_count();        // last result count
const ScanEntry*  scan_entries();      // pointer to N-entry array

// ----- Credential management ---------------------------------------
// Replace stored credentials, persist to /sdcard/config.json, and
// re-trigger a connection attempt. Empty `ssid` disables WiFi.
void set_credentials(const char* ssid, const char* password);

} // namespace wifi_mgr
