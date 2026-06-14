#include "ui/screen_manager.h"
#include "ui/theme.h"
#include "ui/status_bar.h"
#include "ui/anomaly_modal.h"
#include "ui/locate_overlay.h"
#include "ui/wifi_password_modal.h"
#include "ui/text_entry_modal.h"
#include "ui/app_state.h"
#include "ui/screens/screens.h"

namespace ui {

using namespace theme;

// We cache all screens in PSRAM after first build. Navigation then
// becomes a hide/show toggle instead of a destroy-and-rebuild. This
// avoids the big PSRAM allocation burst that was starving the LCD
// DMA on each click (the visible "wobble" during transitions).
//
// Screens that genuinely have to redraw on every navigate (Pick
// Active during a job, Load when state changes) call rebuild_current()
// from their handlers; everything else is static and stays cached.

static lv_obj_t* g_root = nullptr;
static lv_obj_t* g_bodies[(int)Screen::Count] = {};
static bool      g_dirty[(int)Screen::Count]  = {};   // needs rebuild
static Screen    g_current = Screen::Home;

static constexpr int HISTORY_DEPTH = 8;
static Screen g_history[HISTORY_DEPTH];
static int    g_history_len = 0;

static const char* title_for(Screen s) {
    switch (s) {
        case Screen::Home:           return "Reel Rack";
        case Screen::Load:           return "Load";
        case Screen::View:           return "Inventory";
        case Screen::RackGrid:       return "Rack";
        case Screen::PickList:       return "Pick Jobs";
        case Screen::PickActive:     return "Picking";
        case Screen::Configure:      return "Settings";
        case Screen::ConfigSlots:    return "Slot Configuration";
        case Screen::ConfigNetwork:  return "Network";
        case Screen::ConfigSelftest: return "Self Test";
        case Screen::ConfigFwupdate: return "Firmware Update";
        case Screen::ConfigDividers: return "Divider Maintenance";
        case Screen::QrScanner:      return "QR Scanner";
        default:                     return "";
    }
}

static void build_into(Screen s, lv_obj_t* body) {
    switch (s) {
        case Screen::Home:           screens::build_home(body);            break;
        case Screen::Load:           screens::build_load(body);            break;
        case Screen::View:           screens::build_view(body);            break;
        case Screen::RackGrid:       screens::build_rack_grid(body);       break;
        case Screen::PickList:       screens::build_pick_list(body);       break;
        case Screen::PickActive:     screens::build_pick_active(body);     break;
        case Screen::Configure:      screens::build_configure(body);       break;
        case Screen::ConfigSlots:    screens::build_config_slots(body);    break;
        case Screen::ConfigNetwork:  screens::build_config_network(body);  break;
        case Screen::ConfigSelftest: screens::build_config_selftest(body); break;
        case Screen::ConfigFwupdate: screens::build_config_fwupdate(body); break;
        case Screen::ConfigDividers: screens::build_config_dividers(body); break;
        case Screen::QrScanner:      screens::build_qr_scanner(body);      break;
        default: break;
    }
}

static lv_obj_t* make_body() {
    lv_obj_t* body = lv_obj_create(g_root);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, layout::STATUS_H);
    lv_obj_set_size(body, layout::SCREEN_W, layout::BODY_H);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    return body;
}

// Screens whose contents reflect live state (WiFi status, pick-job
// progress) are always rebuilt on entry so the user never sees a
// cached snapshot.
static bool always_dirty(Screen s) {
    return s == Screen::ConfigNetwork
        || s == Screen::PickActive
        || s == Screen::PickList     // re-fetches jobs from InvenTree
        || s == Screen::RackGrid     // live occupancy
        || s == Screen::View         // reconcile may change contents
        || s == Screen::Home         // rack-count tile, fresh load entry
        || s == Screen::Load;        // always start on a fresh scan
}

static void show(Screen s) {
    int idx = (int)s;
    if (idx < 0 || idx >= (int)Screen::Count) return;
    if (always_dirty(s)) g_dirty[idx] = true;

    // Build / rebuild if needed.
    if (!g_bodies[idx]) {
        g_bodies[idx] = make_body();
        g_dirty[idx]  = true;
    }
    if (g_dirty[idx]) {
        lv_obj_clean(g_bodies[idx]);
        build_into(s, g_bodies[idx]);
        g_dirty[idx] = false;
    }

    // Hide all, show current.
    for (int i = 0; i < (int)Screen::Count; ++i) {
        if (g_bodies[i]) {
            if (i == idx) lv_obj_clear_flag(g_bodies[i], LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag (g_bodies[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    g_current = s;

    status_bar_set_title(title_for(g_current));
    status_bar_set_back_visible(g_current != Screen::Home);
    status_bar_set_online(app::state().online);
    status_bar_set_anomaly_count(app::state().anomaly.kind != app::AnomalyKind::None ? 1 : 0);
}

void init() {
    g_root = lv_obj_create(nullptr);
    lv_obj_remove_style_all(g_root);
    lv_obj_add_style(g_root, const_cast<lv_style_t*>(&theme::s().screen_bg), 0);
    lv_obj_set_size(g_root, layout::SCREEN_W, layout::SCREEN_H);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(g_root);

    status_bar_init();
    anomaly_modal_init();
    locate_overlay_init();
    wifi_password_modal_init();
    text_entry_modal_init();

    g_current = Screen::Home;
    show(g_current);
}

void navigate(Screen s) {
    if (s == g_current) return;
    if (g_history_len < HISTORY_DEPTH) {
        g_history[g_history_len++] = g_current;
    } else {
        for (int i = 1; i < HISTORY_DEPTH; ++i) g_history[i-1] = g_history[i];
        g_history[HISTORY_DEPTH - 1] = g_current;
    }
    show(s);
}

void go_back() {
    if (g_history_len == 0) {
        if (g_current != Screen::Home) show(Screen::Home);
        return;
    }
    show(g_history[--g_history_len]);
}

// Mark the active screen dirty and re-show it. Cheap: only the one
// screen rebuilds, the rest stay cached.
void rebuild_current() {
    int idx = (int)g_current;
    if (idx >= 0 && idx < (int)Screen::Count) g_dirty[idx] = true;
    show(g_current);
}

// Flag every cached screen for rebuild (e.g. after a reel topology /
// divider change) and refresh the one on screen right now.
void mark_all_dirty() {
    for (int i = 0; i < (int)Screen::Count; ++i) g_dirty[i] = true;
    show(g_current);
}

Screen current()           { return g_current; }
lv_obj_t* body_container() { return g_bodies[(int)g_current]; }

} // namespace ui
