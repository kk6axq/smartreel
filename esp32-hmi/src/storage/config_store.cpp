#include "storage/config_store.h"
#include "storage/sdcard.h"

#include <ArduinoJson.h>
#include <esp32-hal-log.h>
#include <string.h>
#include <stdio.h>

namespace config_store {

static Config g_cfg;
static bool   g_loaded_config = false;
static bool   g_loaded_rack   = false;

static constexpr const char* PATH_CONFIG = "/sdcard/config.json";
static constexpr const char* PATH_RACK   = "/sdcard/rack.json";

// ---- Helpers -------------------------------------------------------
static void copy_str(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = 0; return; }
    snprintf(dst, cap, "%s", src);
}

static const char* str_or(const JsonVariantConst& v, const char* fallback) {
    if (v.is<const char*>()) return v.as<const char*>();
    return fallback;
}

// ---- Defaults ------------------------------------------------------
void load_defaults(Config& c) {
    memset(&c, 0, sizeof(c));

    copy_str(c.wifi.ssid,     sizeof(c.wifi.ssid),     "");
    copy_str(c.wifi.password, sizeof(c.wifi.password), "");
    c.wifi.enabled = false;

    copy_str(c.inventree.url,   sizeof(c.inventree.url),   "https://inv.lab.local");
    copy_str(c.inventree.token, sizeof(c.inventree.token), "");
    c.inventree.location_id = 42;

    c.display.brightness_pct = 85;
    c.display.sleep_min      = 5;

    c.behaviour.auto_cancel_sec     = 120;
    c.behaviour.low_stock_threshold = 100;
    c.behaviour.scan_confirm        = true;

    copy_str(c.rack.rack_name, sizeof(c.rack.rack_name), "Lab Rack A");
    c.rack.committed = false;
    memset(c.rack.module_count, 0, sizeof(c.rack.module_count));
    c.rack.dividers.clear();
}

// ---- Parse / serialise --------------------------------------------
static bool parse_config(const char* json, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) {
        log_w("config.json parse failed: %s", err.c_str());
        return false;
    }
    auto& c = g_cfg;

    // ArduinoJson v7: MemberProxy is non-copyable, so we read each
    // field via direct chained subscripts on `doc` rather than binding
    // an intermediate to a local.
    copy_str(c.wifi.ssid,     sizeof(c.wifi.ssid),     str_or(doc["wifi"]["ssid"],     c.wifi.ssid));
    copy_str(c.wifi.password, sizeof(c.wifi.password), str_or(doc["wifi"]["password"], c.wifi.password));
    c.wifi.enabled = doc["wifi"]["enabled"] | c.wifi.enabled;

    copy_str(c.inventree.url,   sizeof(c.inventree.url),   str_or(doc["inventree"]["url"],   c.inventree.url));
    copy_str(c.inventree.token, sizeof(c.inventree.token), str_or(doc["inventree"]["token"], c.inventree.token));
    c.inventree.location_id = doc["inventree"]["location_id"] | c.inventree.location_id;

    c.display.brightness_pct = doc["display"]["brightness_pct"] | c.display.brightness_pct;
    c.display.sleep_min      = doc["display"]["sleep_min"]      | c.display.sleep_min;

    c.behaviour.auto_cancel_sec     = doc["behaviour"]["auto_cancel_sec"]     | c.behaviour.auto_cancel_sec;
    c.behaviour.low_stock_threshold = doc["behaviour"]["low_stock_threshold"] | c.behaviour.low_stock_threshold;
    c.behaviour.scan_confirm        = doc["behaviour"]["scan_confirm"]        | c.behaviour.scan_confirm;
    return true;
}

static bool parse_rack(const char* json, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) {
        log_w("rack.json parse failed: %s", err.c_str());
        return false;
    }
    auto& r = g_cfg.rack;
    copy_str(r.rack_name, sizeof(r.rack_name), str_or(doc["rack_name"], r.rack_name));
    r.committed = doc["committed"] | r.committed;

    auto counts = doc["module_count"].as<JsonArrayConst>();
    int ci = 0;
    for (JsonVariantConst v : counts) {
        if (ci >= app::SlotMap::N_PORTS) break;
        r.module_count[ci++] = (uint8_t)(v.as<int>());
    }

    r.dividers.clear();
    auto pulled = doc["pulled"].as<JsonArrayConst>();
    for (JsonVariantConst v : pulled) {
        r.dividers.set_pulled((uint8_t)(v["p"] | 0), (uint8_t)(v["m"] | 0),
                              (uint8_t)(v["s"] | 0), true);
    }
    return true;
}

// ---- Public API ---------------------------------------------------
bool init() {
    load_defaults(g_cfg);
    g_loaded_config = false;
    g_loaded_rack   = false;

    if (!sdcard::mounted()) {
        log_w("config_store: SD not mounted -- using defaults");
        return false;
    }

    // config.json
    size_t sz = sdcard::file_size(PATH_CONFIG);
    if (sz > 0 && sz < 4096) {
        char buf[4096];
        size_t got = sdcard::read_file(PATH_CONFIG, buf, sizeof(buf));
        if (got > 0) g_loaded_config = parse_config(buf, got);
    } else if (sz == 0) {
        log_i("config_store: %s missing -- using defaults", PATH_CONFIG);
    } else {
        log_w("config_store: %s too large (%u) -- skipping", PATH_CONFIG, (unsigned)sz);
    }

    // rack.json
    sz = sdcard::file_size(PATH_RACK);
    if (sz > 0 && sz < 4096) {
        char buf[4096];
        size_t got = sdcard::read_file(PATH_RACK, buf, sizeof(buf));
        if (got > 0) g_loaded_rack = parse_rack(buf, got);
    } else if (sz == 0) {
        log_i("config_store: %s missing -- using defaults", PATH_RACK);
    }

    log_i("config_store: config=%s rack=%s",
          g_loaded_config ? "loaded" : "default",
          g_loaded_rack   ? "loaded" : "default");
    return g_loaded_config || g_loaded_rack;
}

Config& cfg() { return g_cfg; }
bool   loaded() { return g_loaded_config || g_loaded_rack; }

bool save_config() {
    if (!sdcard::mounted()) {
        log_w("save_config: no SD card");
        return false;
    }
    JsonDocument doc;
    auto& c = g_cfg;

    auto wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"]     = c.wifi.ssid;
    wifi["password"] = c.wifi.password;
    wifi["enabled"]  = c.wifi.enabled;

    auto inv = doc["inventree"].to<JsonObject>();
    inv["url"]         = c.inventree.url;
    inv["token"]       = c.inventree.token;
    inv["location_id"] = c.inventree.location_id;

    auto disp = doc["display"].to<JsonObject>();
    disp["brightness_pct"] = c.display.brightness_pct;
    disp["sleep_min"]      = c.display.sleep_min;

    auto bhv = doc["behaviour"].to<JsonObject>();
    bhv["auto_cancel_sec"]     = c.behaviour.auto_cancel_sec;
    bhv["low_stock_threshold"] = c.behaviour.low_stock_threshold;
    bhv["scan_confirm"]        = c.behaviour.scan_confirm;

    char buf[2048];
    size_t n = serializeJsonPretty(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        log_e("save_config: serialise overflow");
        return false;
    }
    return sdcard::write_file_atomic(PATH_CONFIG, buf, n);
}

bool save_rack() {
    if (!sdcard::mounted()) {
        log_w("save_rack: no SD card");
        return false;
    }
    JsonDocument doc;
    auto& r = g_cfg.rack;
    doc["rack_name"] = r.rack_name;
    doc["committed"] = r.committed;
    auto mc = doc["module_count"].to<JsonArray>();
    for (int p = 0; p < app::SlotMap::N_PORTS; ++p) mc.add(r.module_count[p]);
    auto pl = doc["pulled"].to<JsonArray>();
    for (int i = 0; i < r.dividers.n_pulled; ++i) {
        auto o = pl.add<JsonObject>();
        o["p"] = r.dividers.pulled[i].port;
        o["m"] = r.dividers.pulled[i].module;
        o["s"] = r.dividers.pulled[i].slot;
    }

    char buf[1024];
    size_t n = serializeJsonPretty(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        log_e("save_rack: serialise overflow");
        return false;
    }
    return sdcard::write_file_atomic(PATH_RACK, buf, n);
}

void apply_to_app_state() {
    auto& c = g_cfg;
    auto& s = app::state();
    // Only overlay fields that have a real value from disk so that
    // running without an SD card preserves the mock data.
    if (c.wifi.ssid[0])         snprintf(s.wifi_ssid, sizeof(s.wifi_ssid), "%s", c.wifi.ssid);
    if (c.inventree.url[0])     snprintf(s.inv_url,   sizeof(s.inv_url),   "%s", c.inventree.url);
    if (c.inventree.location_id) s.inv_location_id = c.inventree.location_id;
}

} // namespace config_store
