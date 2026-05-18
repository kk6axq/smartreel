// =====================================================================
//  CH422G I2C output expander (driver for the Waveshare 4.3B variant)
//
//  The CH422G has an 8-bit output port. On this board it controls the
//  LCD reset, LCD backlight, touch reset, SD CS line, and a USB switch.
//  Inputs / IRQ pins are not used here.
//
//  The chip is unusual: writing to address 0x24 sets the OC output
//  register, but it requires a one-time mode write to 0x48 to enable
//  push-pull mode. We do that on init() once.
// =====================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

namespace ch422g {

// Initialise the chip and set all bits to a safe default
// (backlight off, all resets asserted, SD CS deasserted high).
bool init();

// Drive a single output pin (0..7).
void write_pin(uint8_t bit, bool level);

// Read back the cached output port mirror (the chip is write-only).
uint8_t read_port_cache();

// Convenience helpers for the things this board cares about.
void lcd_reset(bool released);    // released=true -> reset line HIGH
void lcd_backlight(bool on);
void touch_reset(bool released);

} // namespace ch422g
