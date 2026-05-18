// =====================================================================
//  Anomaly modal -- raised on top of every screen.
//
//  Lives on lv_layer_top() (after the status bar so it draws on top).
//  Single instance, shown/hidden as the anomaly state in app::state()
//  changes.
// =====================================================================
#pragma once

#include <lvgl.h>
#include "ui/app_state.h"

namespace ui {

void anomaly_modal_init();

// Re-render from app::state().anomaly and show.
void anomaly_modal_open_current();

// Hide without clearing app::state().anomaly. The status-bar badge
// stays so the user can re-open.
void anomaly_modal_dismiss();

// Hide and clear the anomaly entirely.
void anomaly_modal_resolve();

// Programmatic raise (e.g. from a self-test button).
void anomaly_modal_raise(app::AnomalyKind kind);

} // namespace ui
