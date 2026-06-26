#include "board/ch422g.h"
#include "board/board_pins.h"
#include "board/i2c_bus.h"

#include <Wire.h>
#include <esp32-hal-log.h>

namespace ch422g {

static uint8_t s_port_cache = 0xFF;   // default-on for active-low signals

// CH422G "I2C addresses" -- the chip mux's its registers by address.
//   0x24  = system / output-enable register. Write 0x01 to enable
//           the IO outputs (otherwise pins stay high-Z and backlight
//           drivers etc. never get a level).
//   0x38  = 8-bit IO output data port (IO0..IO7).
//   0x26  = 8-bit IO input data port (read).
//   0x23  = 4-bit OC output port (open-collector OC0..OC3) - unused.
static constexpr uint8_t REG_OUTPUT_ENABLE = 0x24;
static constexpr uint8_t REG_IO_PORT       = 0x38;

static bool write_raw(uint8_t addr7, uint8_t data) {
    i2c_bus::Lock _g;             // shares the Wire bus with touch + scanner
    Wire.beginTransmission(addr7);
    Wire.write(data);
    return Wire.endTransmission() == 0;
}

bool init() {
    // Step 1: enable IO outputs. Without this the IO pins stay
    // high-impedance no matter what we write to the data register.
    if (!write_raw(REG_OUTPUT_ENABLE, 0x01)) {
        log_e("CH422G: output-enable write failed (chip present?)");
        return false;
    }

    // Step 2: drive a safe initial port state.
    //   bit0..7 = 1 (active-low signals deasserted, lines high)
    //   except LCD backlight off (active-high) and touch reset asserted
    //   so the GT911 can latch its address on release.
    s_port_cache = 0xFF;
    s_port_cache &= ~(1 << EXIO_LCD_BL_BIT);     // backlight off
    s_port_cache &= ~(1 << EXIO_TOUCH_RST_BIT);  // touch held in reset
    s_port_cache &= ~(1 << EXIO_LCD_RST_BIT);    // lcd held in reset

    if (!write_raw(REG_IO_PORT, s_port_cache)) {
        log_e("CH422G: port write failed");
        return false;
    }
    return true;
}

void write_pin(uint8_t bit, bool level) {
    if (bit > 7) return;
    if (level) s_port_cache |= (1 << bit);
    else       s_port_cache &= ~(1 << bit);
    write_raw(REG_IO_PORT, s_port_cache);
}

uint8_t read_port_cache() { return s_port_cache; }

void lcd_reset(bool released)    { write_pin(EXIO_LCD_RST_BIT,   released); }
void lcd_backlight(bool on)      { write_pin(EXIO_LCD_BL_BIT,    on); }
void touch_reset(bool released)  { write_pin(EXIO_TOUCH_RST_BIT, released); }

} // namespace ch422g
