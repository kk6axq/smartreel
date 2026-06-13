// =====================================================================
//  SmartReel Core -- reel hardware abstraction
//
//  Owns the three per-port subsystems of the Core PCB and turns them
//  into clean state + change callbacks the RS485 layer can serve:
//
//    - WS2812B LEDs   (Adafruit_NeoPXL8, PIO+DMA, non-blocking) with a
//                     staged back-buffer per port and an atomic commit.
//    - 74HC165 inputs sampled at cfg::INPUT_SAMPLE_HZ, debounced per
//                     32-bit module word, edge-detected -> input cb.
//    - ADS1115 sense  round-robin per channel -> module count per port,
//                     change -> module-count cb.
//
//  No RS485 knowledge lives here; main.cpp wires the callbacks to the
//  event queue. All calls run on the single application core.
// =====================================================================
#pragma once

#include <stdint.h>
#include "config.h"

namespace reel_hw {

struct PortState {
    uint8_t  module_count;                          // modules sensed on this port
    uint16_t sense_mv;                              // last rail reading (mV)
    uint32_t inputs[cfg::MAX_MODULES_PER_PORT];     // debounced 32-bit state per module
};

// Fired from sample_inputs() when a debounced module word changes.
typedef void (*InputChangeCb)(uint8_t port, uint8_t module,
                              uint32_t prev, uint32_t now);
// Fired from sample_sense() when a port's module count changes (stable).
typedef void (*ModuleCountCb)(uint8_t port, uint8_t prev_count,
                              uint8_t now_count, uint16_t sense_mv);

void begin();
void set_input_change_cb(InputChangeCb cb);
void set_module_count_cb(ModuleCountCb cb);

// ---- Sampling (call from loop on their own cadences) ---------------
void sample_inputs();   // every cfg::INPUT_SAMPLE_MS
void sample_sense();    // every cfg::SENSE_SAMPLE_MS (steps one ADC channel)

// ---- State accessors (for RS485 read handlers) --------------------
const PortState& port(uint8_t p);
uint8_t  reels_present_bitmap();   // bit p set if module_count[p] > 0
bool     ads_ok();

// ---- LEDs ----------------------------------------------------------
// Stage RGB triples into a port's back buffer starting at start_idx.
// Out-of-range pixels are clipped. Nothing lights until commit().
void stage_pixels(uint8_t port, uint16_t start_idx,
                  const uint8_t* rgb, uint16_t count);
void stage_fill(uint8_t port, uint8_t r, uint8_t g, uint8_t b);
// brightness: per-port 0..3, or cfg-wide via REEL_ID_ALL (0xFF).
void set_brightness(uint8_t port_or_all, uint8_t brightness);
// Copy staged buffers for the selected ports into the driver and do a
// single atomic show() across all strips.
void commit(uint8_t port_bitmap);

} // namespace reel_hw
