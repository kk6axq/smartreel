// =====================================================================
//  SmartReel Core -- hardware bring-up test (RP2040)
//
//  Throwaway diagnostic that drives the slot hardware DIRECTLY (no RS485,
//  no protocol). Confirms three subsystems on the Core PCB:
//
//    1. WS2812B LEDs   -- one data line per slot: GPIO6/7/8/9 (slots 1-4)
//    2. ADS1115 ADC    -- I2C0 SDA=GPIO0 SCL=GPIO1, AIN0-3 = reel detect 1-4
//    3. 74HC165 PISO   -- shared SCK=GPIO10, LATCH=GPIO11;
//                         DIN per slot = GPIO12/13/14/15 (slots 1-4)
//
//  Reel-detect divider (per slot rail):
//    2.2k pull-up to 3.3V, and each inserted reel module adds a 10k
//    pull-down. So:
//        empty rail        -> ~3.30 V
//        1 reel  (10k)     -> 3.3 * 10/(10+2.2)       = 2.70 V
//        2 reels (5k)      -> 3.3 * 5/(5+2.2)         = 2.29 V
//        3 reels (3.33k)   -> 3.3 * 3.33/(3.33+2.2)   = 1.99 V
//        4 reels (2.5k)    -> 3.3 * 2.5/(2.5+2.2)     = 1.76 V
//    "present" here just means the rail was pulled below PRESENT_THRESH_MV.
//
//  Everything prints to the USB CDC console at 115200. Type 'help'.
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_ADS1X15.h>

// ---- Pin map -------------------------------------------------------
static constexpr uint8_t N_SLOTS = 4;

static const uint8_t LED_PIN[N_SLOTS] = { 6, 7, 8, 9 };       // WS2812B data
static const uint8_t SR_DIN[N_SLOTS]  = { 12, 13, 14, 15 };   // 74HC165 QH out
static constexpr uint8_t SR_SCK   = 10;   // shared shift clock  (74HC165 CP)
static constexpr uint8_t SR_LATCH = 11;   // shared parallel load (74HC165 PL#, active LOW)

static constexpr uint8_t I2C_SDA = 0;     // I2C0 -> ADS1115
static constexpr uint8_t I2C_SCL = 1;
static constexpr uint8_t ADS_ADDR = 0x48; // ADDR pin -> GND

// ---- Tunables ------------------------------------------------------
// Pixels per slot strip: 16 WS2812B LEDs per reel module.
static constexpr uint16_t LEDS_PER_SLOT = 16;

// Bits clocked out of each slot's 74HC165 chain: 32 buttons per reel
// module (four chained '165s).
static constexpr uint8_t SR_BITS = 32;

// Rail voltage (mV) below which we call a slot "occupied". Empty ~3300mV,
// one reel ~2700mV -> 3000mV sits comfortably between.
static constexpr uint16_t PRESENT_THRESH_MV = 3000;

// LED current limiter -- keep the bench supply happy during the test.
static constexpr uint8_t LED_BRIGHTNESS = 40;   // 0-255

// ---- Globals -------------------------------------------------------
static Adafruit_NeoPixel s_led[N_SLOTS] = {
    Adafruit_NeoPixel(LEDS_PER_SLOT, LED_PIN[0], NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(LEDS_PER_SLOT, LED_PIN[1], NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(LEDS_PER_SLOT, LED_PIN[2], NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(LEDS_PER_SLOT, LED_PIN[3], NEO_GRB + NEO_KHZ800),
};

static Adafruit_ADS1115 s_ads;
static bool s_ads_ok = false;

static bool s_auto = true;   // continuous monitor on/off

// ---- WS2812B helpers -----------------------------------------------
static void led_fill(int slot, uint32_t color) {
    for (uint16_t i = 0; i < LEDS_PER_SLOT; ++i) s_led[slot].setPixelColor(i, color);
    s_led[slot].show();
}

static void led_fill_all(uint32_t color) {
    for (int s = 0; s < N_SLOTS; ++s) led_fill(s, color);
}

// Walk every slot through R/G/B/W so each strip and data line is proven.
static void led_self_test() {
    struct { const char* name; uint32_t rgb; } steps[] = {
        { "RED",   Adafruit_NeoPixel::Color(255, 0,   0)   },
        { "GREEN", Adafruit_NeoPixel::Color(0,   255, 0)   },
        { "BLUE",  Adafruit_NeoPixel::Color(0,   0,   255) },
        { "WHITE", Adafruit_NeoPixel::Color(255, 255, 255) },
    };
    Serial.println(F("[led] self-test: each slot R/G/B/W in turn..."));
    for (auto& step : steps) {
        for (int s = 0; s < N_SLOTS; ++s) {
            led_fill_all(0);
            led_fill(s, step.rgb);
            Serial.printf("[led]   slot %d -> %s\n", s + 1, step.name);
            delay(250);
        }
    }
    led_fill_all(0);
    Serial.println(F("[led] self-test done (all off)"));
}

// ---- ADS1115 reel-detect -------------------------------------------
// Returns rail voltage in mV, or -1 if the ADC isn't responding.
static int read_rail_mv(int slot) {
    if (!s_ads_ok) return -1;
    int16_t raw = s_ads.readADC_SingleEnded(slot);   // AINx, slot==channel
    float volts = s_ads.computeVolts(raw);
    return (int)(volts * 1000.0f + 0.5f);
}

static void read_reel_detect(bool print) {
    if (!s_ads_ok) { if (print) Serial.println(F("[adc] ADS1115 not present -- skipped")); return; }
    if (print) Serial.println(F("[adc] reel detect (AIN0-3):"));
    for (int s = 0; s < N_SLOTS; ++s) {
        int mv = read_rail_mv(s);
        bool present = (mv >= 0 && mv < PRESENT_THRESH_MV);
        if (print) Serial.printf("[adc]   slot %d (AIN%d): %4d mV  %s\n",
                                 s + 1, s, mv, present ? "** REEL PRESENT **" : "(empty)");
    }
}

// ---- 74HC165 PISO --------------------------------------------------
// SCK + LATCH are shared across all four slots, so one load + clock
// sequence reads all four DIN lines at once.
//
// Bit order: the FIRST bit clocked out lands in out-bit 0, the next in
// bit 1, ... So out-bit i == chain position i, and (per the PCB) bit 0 is
// the leftmost divider, bits increase to the right. Layout per module:
//     bit 2k   = divider Dk   (left of reel slot k)
//     bit 2k+1 = reel slot Sk
// i.e. D0 S0 D1 S1 ... D15 S15 over bits 0..31.
static void read_shift_registers(uint32_t out[N_SLOTS]) {
    for (int s = 0; s < N_SLOTS; ++s) out[s] = 0;

    // Parallel load: pulse PL# low, then return high to enter shift mode.
    digitalWrite(SR_LATCH, LOW);
    delayMicroseconds(5);
    digitalWrite(SR_LATCH, HIGH);
    delayMicroseconds(5);

    for (int b = 0; b < SR_BITS; ++b) {
        for (int s = 0; s < N_SLOTS; ++s) {
            if (digitalRead(SR_DIN[s])) out[s] |= (1ul << b);
        }
        digitalWrite(SR_SCK, HIGH);   // shift next bit toward QH
        delayMicroseconds(5);
        digitalWrite(SR_SCK, LOW);
        delayMicroseconds(5);
    }
}

// The four reel-slot cells inside each 8-bit '165 come out in reverse
// spatial order, so physical pixel j shows logical reel slot phys_pixel(j)
// (and vice-versa -- it's its own inverse within each group of 4).
static inline uint16_t phys_pixel(uint16_t j) {
    return (j & ~0x3u) | (3u - (j & 0x3u));
}

// Map one module's 32 button bits onto its 16 WS2812B pixels, working in
// PHYSICAL pixel order so divider colours stay correct across the group-of-4
// boundaries (a logical-neighbour model scatters the shared boundary divider
// ~8 pixels away). For physical pixel j showing reel slot k = phys_pixel(j):
//   G = reel slot k pressed                         (bit 2k+1)
//   R = divider physically to the RIGHT of pixel j  (bit 2k)
//   B = divider physically to the LEFT  of pixel j  (right-divider of pixel j-1)
// i.e. a pressed divider lights its left pixel red and its right pixel blue.
// Additive, so slot+both-dividers = white. The strip ends have no outboard
// divider (pixel 0 no left, pixel 15 no right).
static void update_leds_from_buttons(int mod, uint32_t bits) {
    for (uint16_t j = 0; j < LEDS_PER_SLOT; ++j) {
        uint16_t k = phys_pixel(j);
        bool slot  = bits & (1ul << (2 * k + 1));
        bool right = (j < LEDS_PER_SLOT - 1) && (bits & (1ul << (2 * k)));
        bool left  = (j > 0) && (bits & (1ul << (2 * phys_pixel(j - 1))));
        s_led[mod].setPixelColor(j, s_led[mod].Color(right ? 255 : 0,
                                                     slot  ? 255 : 0,
                                                     left  ? 255 : 0));
    }
    s_led[mod].show();
}

static void print_shift_registers() {
    uint32_t sr[N_SLOTS];
    read_shift_registers(sr);
    Serial.println(F("[sr ] 74HC165 inputs (32 buttons/slot):"));
    for (int s = 0; s < N_SLOTS; ++s) {
        Serial.printf("[sr ]   slot %d (DIN GPIO%d): 0x%08lX  ",
                      s + 1, SR_DIN[s], (unsigned long)sr[s]);
        // Print bit 0 first (leftmost) so it reads in physical order:
        // D0 S0 D1 S1 ... left-to-right.
        for (int b = 0; b < SR_BITS; ++b) {
            Serial.print((sr[s] >> b) & 1);
            if (b % 8 == 7) Serial.print(' ');
        }
        Serial.println();
    }
}

// ---- Console -------------------------------------------------------
static void print_help() {
    Serial.println(F(
        "\nSmartReel Core HW test -- console commands:\n"
        "  help               this text\n"
        "  scan               one-shot: read ADC + shift registers now\n"
        "  led                re-run the R/G/B/W LED self-test\n"
        "  on  <slot> <hex>   fill a slot's LEDs with 0xRRGGBB (slot 1-4)\n"
        "  off [slot]         all LEDs off, or just one slot\n"
        "  auto               toggle the 1 Hz console dump (default ON)\n"
        "LEDs continuously follow the buttons (~30 Hz): per pixel,\n"
        "  reel slot pressed = GREEN, left divider = RED, right divider = BLUE\n"
        "  (additive; all three = white). 'on'/'off' override until the next\n"
        "  refresh. The 1 Hz dump still prints ADC + raw button bits."));
}

static long tok_num(const char* t, long def) {
    if (!t || !*t) return def;
    return strtol(t, nullptr, 0);
}

static void process_line(char* line) {
    char* cmd = strtok(line, " \t");
    if (!cmd) return;

    if (!strcmp(cmd, "help")) { print_help(); return; }
    if (!strcmp(cmd, "scan")) { read_reel_detect(true); print_shift_registers(); return; }
    if (!strcmp(cmd, "led"))  { led_self_test(); return; }
    if (!strcmp(cmd, "auto")) {
        s_auto = !s_auto;
        Serial.printf("[ctl] continuous monitor %s\n", s_auto ? "ON" : "OFF");
        return;
    }
    if (!strcmp(cmd, "on")) {
        int slot = (int)tok_num(strtok(nullptr, " \t"), 0);   // 1-based
        long rgb = tok_num(strtok(nullptr, " \t"), 0xFFFFFF);
        if (slot < 1 || slot > N_SLOTS) { Serial.println(F("usage: on <slot 1-4> <0xRRGGBB>")); return; }
        led_fill(slot - 1, (uint32_t)rgb);
        Serial.printf("[led] slot %d -> 0x%06lX\n", slot, (unsigned long)(rgb & 0xFFFFFF));
        return;
    }
    if (!strcmp(cmd, "off")) {
        char* a = strtok(nullptr, " \t");
        if (!a) { led_fill_all(0); Serial.println(F("[led] all off")); return; }
        int slot = (int)tok_num(a, 0);
        if (slot < 1 || slot > N_SLOTS) { Serial.println(F("usage: off [slot 1-4]")); return; }
        led_fill(slot - 1, 0);
        Serial.printf("[led] slot %d off\n", slot);
        return;
    }
    Serial.printf("unknown command '%s' -- type 'help'\n", cmd);
}

static void poll_console() {
    static char buf[96];
    static size_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') { buf[len] = 0; if (len) process_line(buf); len = 0; continue; }
        if (len < sizeof(buf) - 1) buf[len++] = c;
    }
}

// ---- Arduino entry points ------------------------------------------
void setup() {
    Serial.begin(115200);

    // Shift-register control lines.
    pinMode(SR_SCK, OUTPUT);   digitalWrite(SR_SCK, LOW);
    pinMode(SR_LATCH, OUTPUT); digitalWrite(SR_LATCH, HIGH);   // PL# idle high
    for (int s = 0; s < N_SLOTS; ++s) pinMode(SR_DIN[s], INPUT);

    // WS2812B strips.
    for (int s = 0; s < N_SLOTS; ++s) {
        s_led[s].begin();
        s_led[s].setBrightness(LED_BRIGHTNESS);
        s_led[s].clear();
        s_led[s].show();
    }

    // ADS1115 on I2C0.
    Wire.setSDA(I2C_SDA);
    Wire.setSCL(I2C_SCL);
    Wire.begin();
    s_ads_ok = s_ads.begin(ADS_ADDR, &Wire);
    if (s_ads_ok) {
        // GAIN_ONE = +/-4.096V full scale: covers the 3.3V rail with good
        // resolution (125 uV/LSB).
        s_ads.setGain(GAIN_ONE);
    }

    // Give the USB CDC a moment to come up so the banner isn't lost, but
    // don't block forever if no terminal is attached.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { /* wait */ }

    Serial.println(F("\n[boot] SmartReel Core HARDWARE TEST"));
    Serial.printf("[boot] LEDs GPIO%u/%u/%u/%u (%u px/slot)  SR SCK=%u LATCH=%u DIN=%u/%u/%u/%u (%u bits)\n",
                  LED_PIN[0], LED_PIN[1], LED_PIN[2], LED_PIN[3], LEDS_PER_SLOT,
                  SR_SCK, SR_LATCH, SR_DIN[0], SR_DIN[1], SR_DIN[2], SR_DIN[3], SR_BITS);
    Serial.printf("[boot] ADS1115 @0x%02X on I2C SDA=%u SCL=%u -- %s\n",
                  ADS_ADDR, I2C_SDA, I2C_SCL,
                  s_ads_ok ? "FOUND" : "NOT FOUND (check wiring/address)");

    led_self_test();
    read_reel_detect(true);
    print_shift_registers();
    print_help();
}

void loop() {
    poll_console();

    // Drive LEDs from the buttons at ~30 Hz so presses feel live:
    // per pixel, left divider = red, reel slot = green, right divider = blue.
    static uint32_t last_led = 0;
    if ((millis() - last_led) >= 33) {
        last_led = millis();
        uint32_t sr[N_SLOTS];
        read_shift_registers(sr);
        for (int s = 0; s < N_SLOTS; ++s) update_leds_from_buttons(s, sr[s]);
    }

    // Periodic console dump of ADC + raw button bits.
    static uint32_t last_dump = 0;
    if (s_auto && (millis() - last_dump) >= 1000) {
        last_dump = millis();
        Serial.println(F("------------------------------------------------"));
        read_reel_detect(true);
        print_shift_registers();
    }
}
