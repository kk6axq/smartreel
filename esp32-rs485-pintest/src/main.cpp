// =====================================================================
//  ESP32-S3 RS485 TX-pin toggle bench tool
//
//  Slowly toggles GPIO16 (the RS485 TX line / auto-direction control)
//  every 5 s so it can be followed with a multimeter, and exposes a
//  tiny USB-serial console to hold the pin or jump back to download
//  mode for the next flash.
//
//  Expected on the HMI's RS485 (per the board's auto-direction circuit):
//    GPIO16 HIGH -> SP3485 driver OFF -> bus = bias mark  (A-B ~ +3.8V)
//    GPIO16 LOW  -> SP3485 driver ON, DI=GND -> drives space (A-B < 0)
//  So A-B should flip sign with each toggle if the transceiver works.
// =====================================================================

#include <Arduino.h>
#include "soc/rtc_cntl_reg.h"

static constexpr uint8_t PIN_RS485_TX = 44;   // ESP32-S3 UART0 TX -> RS485

static bool     s_auto   = true;     // auto-toggle vs. held
static bool     s_level  = true;     // current GPIO16 level
static uint32_t s_last   = 0;

static void apply(bool level) {
    s_level = level;
    digitalWrite(PIN_RS485_TX, level ? HIGH : LOW);
    Serial.printf("[pin] GPIO16 = %s  -> SP3485 driver %s, bus %s\n",
                  level ? "HIGH" : "LOW",
                  level ? "OFF" : "ON",
                  level ? "= bias mark (A>B, ~+3.8V)"
                        : "driven to space (A<B, negative)");
}

static void enter_download_mode() {
    Serial.println("[console] rebooting into UART download mode...");
    Serial.flush();
    delay(50);
    REG_WRITE(RTC_CNTL_OPTION1_REG, 0x1);   // force download boot, one-shot
    esp_restart();
}

static void handle_line(const char* line) {
    if (!*line) return;
    if (!strcmp(line, "dl") || !strcmp(line, "bootloader")) { enter_download_mode(); return; }
    if (!strcmp(line, "hi"))   { s_auto = false; apply(true);  return; }
    if (!strcmp(line, "lo"))   { s_auto = false; apply(false); return; }
    if (!strcmp(line, "auto")) { s_auto = true;  Serial.println("[console] auto-toggle every 5s"); return; }
    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        Serial.println("[console] commands:");
        Serial.println("  hi     hold GPIO16 HIGH (driver off / bias mark)");
        Serial.println("  lo     hold GPIO16 LOW  (driver on / drives space)");
        Serial.println("  auto   resume 5s auto-toggle");
        Serial.println("  dl     reboot into UART download mode");
        return;
    }
    Serial.printf("[console] unknown: '%s' (try 'help')\n", line);
}

static void poll_console() {
    static char buf[48];
    static size_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') { buf[len] = 0; if (len) handle_line(buf); len = 0; continue; }
        if (len < sizeof(buf) - 1) buf[len++] = c;
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);
    pinMode(PIN_RS485_TX, OUTPUT);
    apply(true);                 // start HIGH = driver off / idle mark
    s_last = millis();
    Serial.println("\n[boot] RS485 TX-pin toggle tool");
    Serial.println("[boot] toggling GPIO16 every 5s; type 'help' for manual control");
}

void loop() {
    if (s_auto && millis() - s_last >= 5000) {
        s_last = millis();
        apply(!s_level);
    }
    poll_console();
    delay(10);
}
