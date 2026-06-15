#include "app/beeper.h"

#include <Arduino.h>
#include <lvgl.h>

namespace beeper {

// -1 = no buzzer wired yet (review item 5: hardware is added later). Set
// this to the GPIO the passive buzzer is driven from and the tones below
// come alive with no other change. A passive piezo on a free GPIO via the
// LEDC PWM peripheral is the expected wiring.
#ifndef BEEPER_PIN
#define BEEPER_PIN (-1)
#endif

namespace {
constexpr int      LEDC_CH   = 7;     // LEDC channel reserved for the buzzer
constexpr uint8_t  LEDC_RES  = 10;    // bits (tone API ignores duty depth)
bool g_ready = false;

#if BEEPER_PIN >= 0
// Stop the tone after its scheduled duration. One-shot lv_timer so we never
// block the LVGL task with delay().
void stop_cb(lv_timer_t* t) {
    ledcWriteTone(LEDC_CH, 0);
    lv_timer_del(t);
}

void tone(int freq_hz, int ms) {
    if (!g_ready) return;
    ledcWriteTone(LEDC_CH, freq_hz);
    lv_timer_t* t = lv_timer_create(stop_cb, ms, nullptr);
    lv_timer_set_repeat_count(t, 1);
}
#else
inline void tone(int, int) {}   // no pin: compiled out
#endif
} // namespace

void init() {
#if BEEPER_PIN >= 0
    ledcSetup(LEDC_CH, 2000, LEDC_RES);
    ledcAttachPin(BEEPER_PIN, LEDC_CH);
    ledcWriteTone(LEDC_CH, 0);
    g_ready = true;
    Serial.printf("[beeper] ready on GPIO %d\n", BEEPER_PIN);
#else
    Serial.println("[beeper] no pin wired (BEEPER_PIN<0) -- tones log only");
#endif
}

// Frequencies/patterns are placeholders chosen to be distinguishable;
// tune once the buzzer is on the board.
void ok()    { Serial.println("[beeper] ok");    tone(1760, 90); }
void error() { Serial.println("[beeper] error"); tone( 330, 250); }
void warn()  { Serial.println("[beeper] warn");  tone( 880, 150); }
void click() { tone(2200, 25); }

} // namespace beeper
