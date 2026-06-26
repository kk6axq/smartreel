// =====================================================================
//  SmartReel Core -- reel hardware abstraction (implementation)
//
//  See reel_hw.h for the interface and config.h for the pin map and
//  the divider math behind module_count_from_mv().
// =====================================================================
#include "reel_hw.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPXL8.h>
#include <Adafruit_ADS1X15.h>
#include <string.h>

namespace reel_hw {

using cfg::N_PORTS;
using cfg::MAX_MODULES_PER_PORT;
using cfg::MODULE_LEDS;
using cfg::MAX_PIXELS_PER_PORT;

// ---- LED driver ----------------------------------------------------
// One NeoPXL8 instance drives all four data lines in parallel via PIO+
// DMA. Strips are MAX_PIXELS_PER_PORT long; port p occupies global
// pixels [p*MAX_PIXELS_PER_PORT, +MAX_PIXELS_PER_PORT). We only ever
// fill the active (module_count*MODULE_LEDS) prefix; the rest stays 0.
static int8_t           s_led_pins[8];
static Adafruit_NeoPXL8* s_leds = nullptr;

// Per-port staged back-buffer (RGB) + brightness. COMMIT copies these
// into the driver (applying brightness) and does one show().
static uint8_t  s_staged[N_PORTS][MAX_PIXELS_PER_PORT * 3];
static uint8_t  s_brightness[N_PORTS];

// ---- Sense ADC -----------------------------------------------------
static Adafruit_ADS1115 s_ads;
static bool             s_ads_ok = false;
static uint8_t          s_sense_ch = 0;             // round-robin cursor
// Debounce for the quantized module count, per port.
static uint8_t          s_count_cand[N_PORTS];
static uint8_t          s_count_n[N_PORTS];

// ---- Input debounce ------------------------------------------------
static uint32_t s_in_committed[N_PORTS][MAX_MODULES_PER_PORT];
static uint32_t s_in_cand[N_PORTS][MAX_MODULES_PER_PORT];
static uint8_t  s_in_n[N_PORTS][MAX_MODULES_PER_PORT];

// ---- Public state --------------------------------------------------
static PortState s_port[N_PORTS];

static InputChangeCb  s_input_cb  = nullptr;
static ModuleCountCb  s_count_cb  = nullptr;

void set_input_change_cb(InputChangeCb cb) { s_input_cb = cb; }
void set_module_count_cb(ModuleCountCb cb) { s_count_cb = cb; }

const PortState& port(uint8_t p) { return s_port[p < N_PORTS ? p : 0]; }
bool ads_ok() { return s_ads_ok; }

uint8_t reels_present_bitmap() {
    uint8_t b = 0;
    for (uint8_t p = 0; p < N_PORTS; ++p)
        if (s_port[p].module_count > 0) b |= (1u << p);
    return b;
}

// =====================================================================
//  Init
// =====================================================================
void begin() {
    memset(s_staged, 0, sizeof(s_staged));
    memset(s_in_committed, 0, sizeof(s_in_committed));
    memset(s_in_cand, 0, sizeof(s_in_cand));
    memset(s_in_n, 0, sizeof(s_in_n));
    memset(s_count_cand, 0, sizeof(s_count_cand));
    memset(s_count_n, 0, sizeof(s_count_n));
    for (uint8_t p = 0; p < N_PORTS; ++p) {
        s_brightness[p] = cfg::LED_BRIGHTNESS_DEFAULT;
        s_port[p].module_count = 0;
        s_port[p].sense_mv = (uint16_t)cfg::SENSE_VREF_MV;
        for (uint8_t m = 0; m < MAX_MODULES_PER_PORT; ++m) s_port[p].inputs[m] = 0;
    }

    // 74HC165 control lines (shared clock + latch, one DIN per port).
    pinMode(cfg::SR_SCK, OUTPUT);   digitalWrite(cfg::SR_SCK, LOW);
    pinMode(cfg::SR_LATCH, OUTPUT); digitalWrite(cfg::SR_LATCH, HIGH);  // PL# idle high
    for (uint8_t p = 0; p < N_PORTS; ++p) pinMode(cfg::SR_DIN[p], INPUT);

    // WS2812B via NeoPXL8 (PIO+DMA). pins[] copied to a mutable array.
    memcpy(s_led_pins, cfg::LED_PINS, sizeof(s_led_pins));
    s_leds = new Adafruit_NeoPXL8(MAX_PIXELS_PER_PORT, s_led_pins, NEO_GRB + NEO_KHZ800);
    s_leds->begin();
    s_leds->setBrightness(255);          // we apply our own per-port scaling
    s_leds->clear();
    s_leds->show();

    // ADS1115 on I2C0.
    Wire.setSDA(cfg::I2C_SDA);
    Wire.setSCL(cfg::I2C_SCL);
    Wire.begin();
    s_ads_ok = s_ads.begin(cfg::ADS_ADDR, &Wire);
    if (s_ads_ok) {
        s_ads.setGain(GAIN_ONE);                 // +/-4.096V FS, 125 uV/LSB
        s_ads.setDataRate(RATE_ADS1115_250SPS);  // ~4 ms/conversion
    }
}

// =====================================================================
//  74HC165 input sampling + debounce
// =====================================================================
// Clock the shared chains and capture each port's DIN bit-by-bit.
// Module m occupies clock-positions [m*32, m*32+32); after descrambling
// (below) each module word is the interleaved D0 S0 D1 S1 ... layout.
//
// Hardware quirk: each 8-channel 74HC165 (= one byte = 4 slots) clocks
// its four slots out in REVERSED order, so within every byte the four
// (divider, slot) pairs arrive as slot 3,2,1,0 instead of 0,1,2,3. We
// undo that here -- per byte, reverse the four 2-bit pairs while keeping
// each divider glued to its slot -- so the rest of the system sees the
// clean logical D0 S0 D1 S1 ... layout. (A plain 8-bit flip would also
// swap the D/S bits and decode reels as dividers; we reverse pairs, not
// bits.) Byte/group order is correct as-is, so only within-byte order
// is undone.
static inline uint8_t descramble_bit(uint8_t clocked_bit) {
    const uint8_t byte_i  = clocked_bit / 8;       // which 74HC165 (slots group)
    const uint8_t in_byte = clocked_bit % 8;
    const uint8_t pair    = in_byte / 2;           // slot lane within byte (0..3)
    const uint8_t sub     = in_byte % 2;           // 0 = divider, 1 = slot
    return byte_i * 8 + (3 - pair) * 2 + sub;      // reverse the four pairs
}

static void read_chains_raw(uint32_t raw[N_PORTS][MAX_MODULES_PER_PORT],
                            uint8_t max_modules) {
    for (uint8_t p = 0; p < N_PORTS; ++p)
        for (uint8_t m = 0; m < MAX_MODULES_PER_PORT; ++m) raw[p][m] = 0;

    // Parallel load: pulse PL# low, then high to enter shift mode.
    digitalWrite(cfg::SR_LATCH, LOW);
    delayMicroseconds(2);
    digitalWrite(cfg::SR_LATCH, HIGH);
    delayMicroseconds(2);

    const uint16_t total_bits = (uint16_t)max_modules * cfg::MODULE_INPUT_BITS;
    for (uint16_t b = 0; b < total_bits; ++b) {
        const uint8_t  m   = b / cfg::MODULE_INPUT_BITS;
        const uint8_t  bit = descramble_bit(b % cfg::MODULE_INPUT_BITS);
        for (uint8_t p = 0; p < N_PORTS; ++p) {
            if (digitalRead(cfg::SR_DIN[p])) raw[p][m] |= (1ul << bit);
        }
        digitalWrite(cfg::SR_SCK, HIGH);
        delayMicroseconds(2);
        digitalWrite(cfg::SR_SCK, LOW);
        delayMicroseconds(2);
    }
}

void sample_inputs() {
    // Only clock as far as the most-populated port needs.
    uint8_t max_modules = 0;
    for (uint8_t p = 0; p < N_PORTS; ++p)
        if (s_port[p].module_count > max_modules) max_modules = s_port[p].module_count;
    if (max_modules == 0) return;

    uint32_t raw[N_PORTS][MAX_MODULES_PER_PORT];
    read_chains_raw(raw, max_modules);

    for (uint8_t p = 0; p < N_PORTS; ++p) {
        for (uint8_t m = 0; m < s_port[p].module_count; ++m) {
            const uint32_t v = raw[p][m];
            if (v == s_in_cand[p][m]) {
                if (s_in_n[p][m] < 255) s_in_n[p][m]++;
            } else {
                s_in_cand[p][m] = v;
                s_in_n[p][m]    = 1;
            }
            if (s_in_n[p][m] >= cfg::INPUT_DEBOUNCE_SAMPLES &&
                s_in_committed[p][m] != s_in_cand[p][m]) {
                const uint32_t prev = s_in_committed[p][m];
                s_in_committed[p][m] = s_in_cand[p][m];
                s_port[p].inputs[m]  = s_in_cand[p][m];
                if (s_input_cb) s_input_cb(p, m, prev, s_in_cand[p][m]);
            }
        }
    }
}

// =====================================================================
//  ADS1115 sense sampling -> module count
// =====================================================================
static int read_rail_mv(uint8_t ch) {
    if (!s_ads_ok) return -1;
    int16_t raw = s_ads.readADC_SingleEnded(ch);
    return (int)(s_ads.computeVolts(raw) * 1000.0f + 0.5f);
}

// Wipe debounce + reported state for modules no longer present so a
// re-inserted module starts from a clean (all-released) baseline.
static void reset_modules_from(uint8_t p, uint8_t from) {
    for (uint8_t m = from; m < MAX_MODULES_PER_PORT; ++m) {
        s_in_committed[p][m] = 0;
        s_in_cand[p][m]      = 0;
        s_in_n[p][m]         = 0;
        s_port[p].inputs[m]  = 0;
    }
}

// Seed a port's input state from an immediate raw scan, bypassing the
// debounce ramp. The debounced sampler commits a module word only once a
// candidate has been stable for INPUT_DEBOUNCE_SAMPLES *and* differs from the
// committed value -- which starts at zero. So between "modules detected" and
// the first debounce commit, READ_INPUTS reports all-zero, and the HMI can't
// tell that from a legitimately empty, divider-less module. Priming the moment
// modules appear makes the reported state reflect physical reality at boot
// (review item R5). Called when a port's module count rises (boot or insert).
static void prime_inputs(uint8_t p) {
    const uint8_t mc = s_port[p].module_count;
    if (mc == 0) return;
    uint8_t max_modules = 0;
    for (uint8_t q = 0; q < N_PORTS; ++q)
        if (s_port[q].module_count > max_modules) max_modules = s_port[q].module_count;
    uint32_t raw[N_PORTS][MAX_MODULES_PER_PORT];
    read_chains_raw(raw, max_modules);          // shared chain: reads all ports
    for (uint8_t m = 0; m < mc; ++m) {
        s_in_committed[p][m] = raw[p][m];
        s_in_cand[p][m]      = raw[p][m];
        s_in_n[p][m]         = cfg::INPUT_DEBOUNCE_SAMPLES;   // already settled
        s_port[p].inputs[m]  = raw[p][m];
    }
}

void sample_sense() {
    const uint8_t p = s_sense_ch;
    s_sense_ch = (s_sense_ch + 1) % N_PORTS;

    const int mv = read_rail_mv(p);
    if (mv < 0) return;                 // ADC fault: leave last known state
    s_port[p].sense_mv = (uint16_t)mv;

    const uint8_t count = cfg::module_count_from_mv(mv);
    if (count == s_count_cand[p]) {
        if (s_count_n[p] < 255) s_count_n[p]++;
    } else {
        s_count_cand[p] = count;
        s_count_n[p]    = 1;
    }
    // Require two consecutive agreeing reads before accepting (the
    // divider steps narrow at high counts; this rejects threshold jitter).
    if (s_count_n[p] >= 2 && s_port[p].module_count != s_count_cand[p]) {
        const uint8_t prev = s_port[p].module_count;
        const uint8_t now  = s_count_cand[p];
        s_port[p].module_count = now;
        if (now < prev) reset_modules_from(p, now);   // dropped modules
        // Modules appeared (boot or hot-insert): seed their input state from a
        // real scan now so READ_INPUTS is correct immediately, not after the
        // debounce ramp (review item R5).
        if (now > prev) prime_inputs(p);
        if (s_count_cb) s_count_cb(p, prev, now, s_port[p].sense_mv);
    }
}

// =====================================================================
//  LEDs
// =====================================================================
void stage_pixels(uint8_t port_, uint16_t start_idx,
                  const uint8_t* rgb, uint16_t count) {
    if (port_ >= N_PORTS || !rgb) return;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t idx = start_idx + i;
        if (idx >= MAX_PIXELS_PER_PORT) break;
        uint8_t* dst = &s_staged[port_][idx * 3];
        dst[0] = rgb[i * 3 + 0];
        dst[1] = rgb[i * 3 + 1];
        dst[2] = rgb[i * 3 + 2];
    }
}

void stage_fill(uint8_t port_, uint8_t r, uint8_t g, uint8_t b) {
    if (port_ >= N_PORTS) return;
    for (uint16_t i = 0; i < MAX_PIXELS_PER_PORT; ++i) {
        uint8_t* dst = &s_staged[port_][i * 3];
        dst[0] = r; dst[1] = g; dst[2] = b;
    }
}

void set_brightness(uint8_t port_or_all, uint8_t brightness) {
    if (port_or_all == cfg::PORT_ALL) {
        for (uint8_t p = 0; p < N_PORTS; ++p) s_brightness[p] = brightness;
    } else if (port_or_all < N_PORTS) {
        s_brightness[port_or_all] = brightness;
    }
}

void commit(uint8_t port_bitmap) {
    if (!s_leds) return;
    for (uint8_t p = 0; p < N_PORTS; ++p) {
        if (!(port_bitmap & (1u << p))) continue;
        const uint16_t base = (uint16_t)p * MAX_PIXELS_PER_PORT;
        const uint8_t  bri  = s_brightness[p];
        for (uint16_t i = 0; i < MAX_PIXELS_PER_PORT; ++i) {
            const uint8_t* src = &s_staged[p][i * 3];
            const uint8_t r = (uint16_t)src[0] * bri / 255;
            const uint8_t g = (uint16_t)src[1] * bri / 255;
            const uint8_t b = (uint16_t)src[2] * bri / 255;
            s_leds->setPixelColor(base + i, r, g, b);
        }
    }
    s_leds->show();   // one atomic DMA push across all strips
}

} // namespace reel_hw
