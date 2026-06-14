// =====================================================================
//  Locate overlay -- persistent "go look at this slot" highlight.
//
//  Driven by the InvenTree web-UI "locate" button: the plugin records a
//  pending locate on the rack, the HMI sees it on its GET /rack poll
//  (inv_api::RackResult::locates) and asks this overlay to light the
//  slot(s). Unlike the View screen's "Find" (a fixed 3 s timer), a
//  locate STAYS LIT until an operator dismisses it here -- they may have
//  walked to the rack, so the LED must persist while they're away from
//  the screen.
//
//  Lives on lv_layer_top() like the anomaly modal. LVGL-task only: all
//  functions must be called on the LVGL task (inv_sync routes through
//  ui::dispatch_on_lvgl). Active-locate state is owned here and is NOT
//  re-seeded from the poll -- a later GET /rack with an empty locates[]
//  must not clear an already-active highlight; only Dismiss clears it.
// =====================================================================
#pragma once

namespace ui {

void locate_overlay_init();

// Add a pending locate for `slot_num`: light the slot (persistent) and
// show/refresh the overlay. Idempotent per slot -- a slot already lit is
// not stacked. No-op for an out-of-range slot. LVGL task only.
void locate_overlay_add(int slot_num);

// True while one or more locates are active (LED lit, overlay shown).
bool locate_overlay_active();

} // namespace ui
