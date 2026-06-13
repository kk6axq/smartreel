// =====================================================================
//  Screen manager + navigation history.
//
//  Mirrors the JS router in DisplaySkeleton/index.html.
//  Each screen is rebuilt on every show() (matches the JS renderer
//  pattern) so dynamic data (rack state, pick progress) is always
//  fresh. The 800x440 body container sits below the persistent
//  status bar.
// =====================================================================
#pragma once

#include <lvgl.h>

namespace ui {

enum class Screen {
    Home,
    Load,
    View,
    RackGrid,              // slot-occupancy dot grid (moved off Home)
    PickList,
    PickActive,
    Configure,             // displayed as "Settings"
    ConfigSlots,
    ConfigNetwork,
    ConfigSelftest,
    ConfigFwupdate,
    ConfigDividers,
    QrScanner,             // live scan view, opened from Self Test
    Count,
};

void init();                  // Build status bar, modal, then navigate(Home)
void navigate(Screen s);
void go_back();
void rebuild_current();       // re-run the current screen's renderer
void mark_all_dirty();        // flag every cached screen to rebuild on next show
                              // (and rebuild the current one now)

Screen current();

// Returns the body container for the current screen (used internally
// by screen builders).
lv_obj_t* body_container();

} // namespace ui
