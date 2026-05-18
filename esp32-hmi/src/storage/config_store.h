// =====================================================================
//  Persistent configuration store (JSON on SD card)
//
//  Two files at the SD root:
//      /config.json  -- system config (Wi-Fi, Inventree, display,
//                       sound, behaviour). Editable via the
//                       Configure screens.
//      /rack.json    -- physical rack layout (chains, slots,
//                       numbering, dividers). Edited via Configure ->
//                       Slots / Dividers.
//
//  All file I/O is atomic via sdcard::write_file_atomic so a power
//  loss mid-save never leaves a half-written file.
//
//  If the SD card is missing or files are corrupt, defaults are used
//  and the system reports config_store::loaded() == false. In that
//  state save_config() / save_rack() will still work as soon as a
//  card is inserted and re-init runs.
// =====================================================================
#pragma once

#include "ui/app_state.h"

namespace config_store {

struct WifiCfg {
    char ssid[32];
    char password[64];
    bool enabled;     // skip connect if false (e.g. demo / offline mode)
};

struct InventreeCfg {
    char url[80];
    char token[80];
    int  location_id;
};

struct DisplayCfg {
    int  brightness_pct;
    int  sleep_min;
};

struct BehaviourCfg {
    int  auto_cancel_sec;
    int  low_stock_threshold;
    bool scan_confirm;
};

struct RackCfg {
    int  n_chains;
    int  slots_per_chain;
    char numbering[24];   // "ltr-ttb", "rtl-ttb", "per-chain"
    int  skip_count;
    int  skip_list[8];
    char rack_name[32];
};

struct Config {
    WifiCfg      wifi;
    InventreeCfg inventree;
    DisplayCfg   display;
    BehaviourCfg behaviour;
    RackCfg      rack;
};

// Reset to compiled-in defaults.
void load_defaults(Config& c);

// Read JSON files from the SD card into `cfg()`. Returns true if
// at least one file was successfully parsed; on any failure the
// missing fields keep their default values.
bool init();

// Mutable accessor. Modifying fields in-place is OK; nothing else
// reads them on a separate thread.
Config& cfg();

// Persist the relevant slice of cfg() back to SD. Returns true on
// success. Quietly returns false (and logs a warning) if no SD card.
bool save_config();
bool save_rack();

// True if either file was successfully read at boot.
bool loaded();

// Apply the loaded config into app_state (rack name, fw version
// stays untouched, etc.). Call after init() so the UI uses real
// values.
void apply_to_app_state();

} // namespace config_store
