// =====================================================================
//  SmartReel Core -- production firmware configuration
//
//  Pin map and hardware tunables for the Core PCB (RP2040). The pin
//  assignments here match the bring-up test (core-hwtest) verbatim --
//  that test confirmed all three slot subsystems on real hardware.
//
//  Hardware model
//  --------------
//  Four PORTS (0..3), each a physical connector on the Core PCB:
//      - one WS2812B data line          (GP6/7/8/9)
//      - one ADS1115 sense channel       (AIN0..3)
//      - one 74HC165 shift chain DIN     (GP12/13/14/15)
//        with SHARED clock (GP10) + latch (GP11)
//
//  Each port carries one or more chained reel MODULES. A module is a
//  pluggable strip of 16 reel-slots + 16 dividers:
//      - 16 WS2812B LEDs   (so a port's LED chain = 16 * n_modules)
//      - 32 input bits      (four 74HC165s; port chain = 32 * n_modules,
//        interleaved divider/slot: D0 S0 D1 S1 ... D15 S15)
//
//  The number of modules on a port is sensed from a voltage divider:
//  a 2.2k pull-up to 3.3V, plus a 10k pull-down per inserted module (so
//  n modules present 10k/n in parallel). See module_count_from_mv().
// =====================================================================
#pragma once

#include <stdint.h>

namespace cfg {

// ---- Port topology -------------------------------------------------
static constexpr uint8_t  N_PORTS            = 4;
static constexpr uint8_t  PORT_ALL           = 0xFF;   // "all ports" sentinel (matches rs485::REEL_ID_ALL)
static constexpr uint8_t  MODULE_LEDS        = 16;   // WS2812B per module
static constexpr uint8_t  MODULE_INPUT_BITS  = 32;   // 74HC165 bits per module
static constexpr uint8_t  MAX_MODULES_PER_PORT = 4;  // ADC divider resolution ceiling

static constexpr uint16_t MAX_PIXELS_PER_PORT = MODULE_LEDS * MAX_MODULES_PER_PORT;       // 64
static constexpr uint16_t MAX_BITS_PER_PORT   = MODULE_INPUT_BITS * MAX_MODULES_PER_PORT; // 128

// ---- WS2812B (one data line per port) ------------------------------
// NeoPXL8 needs all 8 pins within an 8-pin window; 6..9 qualifies.
// Unused outputs are -1. Index order = port 0..3.
static constexpr int8_t   LED_PINS[8]   = { 6, 7, 8, 9, -1, -1, -1, -1 };
static constexpr uint8_t  LED_BRIGHTNESS_DEFAULT = 64;   // 0-255, global

// ---- 74HC165 PISO (shared clock + latch, one DIN per port) ---------
static constexpr uint8_t  SR_DIN[N_PORTS] = { 12, 13, 14, 15 };
static constexpr uint8_t  SR_SCK   = 10;   // shared shift clock  (74HC165 CP)
static constexpr uint8_t  SR_LATCH = 11;   // shared parallel load (74HC165 PL#, active LOW)

// ---- ADS1115 reel-sense ADC ----------------------------------------
static constexpr uint8_t  I2C_SDA  = 0;    // I2C0 -> ADS1115
static constexpr uint8_t  I2C_SCL  = 1;
static constexpr uint8_t  ADS_ADDR = 0x48; // ADDR pin -> GND

// Sense divider component values. Rail = 3.3V * Rpd / (Rpd + RPULLUP),
// where Rpd = RMODULE / n_modules (pull-downs in parallel).
static constexpr float    SENSE_VREF_MV  = 3300.0f;
static constexpr float    SENSE_RPULLUP  = 2200.0f;   // ohms
static constexpr float    SENSE_RMODULE  = 10000.0f;  // ohms, per module

// ---- RS485 link ----------------------------------------------------
static constexpr uint8_t  PIN_RS485_TX = 16;   // -> SP3485 DI
static constexpr uint8_t  PIN_RS485_RX = 17;   // <- SP3485 RO
static constexpr uint8_t  PIN_RS485_DE = 18;   // -> SP3485 DE + RE#
// 115200 to match the HMI's auto-direction transceiver (can't turn
// around fast enough for higher rates). See esp32-hmi RS485 notes.
static constexpr uint32_t RS485_BAUD   = 115200;

// ---- Debug LEDs (discrete, on the Core PCB) ------------------------
static constexpr uint8_t  PIN_DBG_LINK = 2;    // RS485 link health
static constexpr uint8_t  PIN_DBG_AUX  = 3;    // spare / activity blip

// ---- Timing --------------------------------------------------------
// Button sampling: comfortably above the >5 Hz floor. 100 Hz gives
// ~10 ms input latency and leaves the bus + LEDs plenty of headroom.
static constexpr uint32_t INPUT_SAMPLE_HZ   = 100;
static constexpr uint32_t INPUT_SAMPLE_MS   = 1000 / INPUT_SAMPLE_HZ;
// A change must persist this many consecutive samples to count (debounce).
static constexpr uint8_t  INPUT_DEBOUNCE_SAMPLES = 2;

// Sense sampling is slow (ADS1115 conversions) and presence changes
// are slow too -- round-robin one channel per tick at ~10 Hz/channel.
static constexpr uint32_t SENSE_SAMPLE_MS   = 25;   // per-channel step; 4 ch -> 10 Hz each

// RS485 link is "up" if a valid master frame arrived within this window.
static constexpr uint32_t LINK_TIMEOUT_MS   = 1000;

// ---- Module count from sense voltage -------------------------------
// Expected rail voltage (mV) for n modules on a port. n==0 is the open
// rail (~Vref). Returned by reference for diagnostics.
inline uint16_t sense_mv_for_count(uint8_t n) {
    if (n == 0) return (uint16_t)(SENSE_VREF_MV + 0.5f);
    float rpd = SENSE_RMODULE / (float)n;
    return (uint16_t)(SENSE_VREF_MV * rpd / (rpd + SENSE_RPULLUP) + 0.5f);
}

// Quantize a measured rail voltage to a module count by nearest expected
// level. mv < 0 (ADC fault) yields 0.
inline uint8_t module_count_from_mv(int mv) {
    if (mv < 0) return 0;
    uint8_t best = 0;
    int     best_err = 32767;
    for (uint8_t n = 0; n <= MAX_MODULES_PER_PORT; ++n) {
        int err = (int)mv - (int)sense_mv_for_count(n);
        if (err < 0) err = -err;
        if (err < best_err) { best_err = err; best = n; }
    }
    return best;
}

} // namespace cfg
