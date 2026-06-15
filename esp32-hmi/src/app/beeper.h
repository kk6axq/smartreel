// =====================================================================
//  Beeper -- semantic audio feedback for confirmations and errors.
//
//  The buzzer hardware is added separately (review item 5: "Add beeper
//  hardware (I'll do this, then we'll wire confirmation and error
//  actions to beep tones)"). This module is the software seam: every
//  confirmation/error site in the UI calls one of the semantic helpers
//  below NOW, so wiring the real tones later is a one-file change here.
//
//  Until a pin is wired (BEEPER_PIN < 0) the calls are safe no-ops that
//  still log to serial, so the intended tone points are observable on
//  the bench without any hardware.
//
//  Threading: tones are started with the LEDC peripheral (non-blocking)
//  and stopped by a one-shot lv_timer, so the helpers are safe to call
//  from the LVGL task. No PRO_CPU work (see memory hmi-cpu-pinning).
// =====================================================================
#pragma once

namespace beeper {

// Configure the LEDC channel if a buzzer pin is wired. Safe to call once
// from setup(); a no-op when BEEPER_PIN < 0.
void init();

// Semantic tones. Wire these to real frequencies/patterns once the
// hardware exists; today they log + (if a pin is set) emit a short tone.
void ok();       // load/pick confirmed, refresh complete
void error();    // scan/resolve error, illegal removal
void warn();     // needs attention (anomaly raised)
void click();    // short tick (button ack)

} // namespace beeper
