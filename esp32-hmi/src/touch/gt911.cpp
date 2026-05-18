#include "touch/gt911.h"
#include "board/board_pins.h"
#include "board/ch422g.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp32-hal-log.h>

namespace touch {

// Register addresses
static constexpr uint16_t REG_PRODUCT_ID  = 0x8140;
static constexpr uint16_t REG_STATUS      = 0x814E;
static constexpr uint16_t REG_POINT1      = 0x8150;

static uint8_t           s_addr = 0;
static lv_indev_drv_t    s_indev_drv;

static bool i2c_read(uint16_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(s_addr);
    Wire.write(uint8_t(reg >> 8));
    Wire.write(uint8_t(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    size_t got = Wire.requestFrom(s_addr, (uint8_t)len);
    if (got != len) return false;
    for (size_t i = 0; i < len; ++i) buf[i] = Wire.read();
    return true;
}

static bool i2c_write(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(s_addr);
    Wire.write(uint8_t(reg >> 8));
    Wire.write(uint8_t(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool init() {
    // Power-on sequence: hold INT low while releasing reset to choose
    // I2C address 0x5D (vs 0x14 with INT high). We pin INT low briefly
    // then leave it as a pure input.
    pinMode(TOUCH_INT_PIN, OUTPUT);
    digitalWrite(TOUCH_INT_PIN, LOW);
    ch422g::touch_reset(false);
    delay(10);
    ch422g::touch_reset(true);
    delay(50);
    pinMode(TOUCH_INT_PIN, INPUT);

    if      (probe(GT911_I2C_ADDR_PRI)) s_addr = GT911_I2C_ADDR_PRI;
    else if (probe(GT911_I2C_ADDR_SEC)) s_addr = GT911_I2C_ADDR_SEC;
    else {
        log_e("GT911 not found on I2C");
        return false;
    }

    uint8_t pid[4] = {};
    if (!i2c_read(REG_PRODUCT_ID, pid, sizeof(pid))) {
        log_w("GT911 ID read failed");
    } else {
        log_i("GT911 product id: %c%c%c%c", pid[0], pid[1], pid[2], pid[3]);
    }

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = read_cb;
    lv_indev_drv_register(&s_indev_drv);
    return true;
}

void read_cb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    static int16_t last_x = 0;
    static int16_t last_y = 0;

    uint8_t status = 0;
    if (!i2c_read(REG_STATUS, &status, 1)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // The high bit means "buffer ready"; low nibble = touch count.
    uint8_t touch_count = status & 0x0F;
    if ((status & 0x80) == 0 || touch_count == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = last_x;
        data->point.y = last_y;
        // Always clear the status reg so the controller can buffer next.
        i2c_write(REG_STATUS, 0);
        return;
    }

    uint8_t pt[8] = {};
    if (!i2c_read(REG_POINT1, pt, sizeof(pt))) {
        data->state = LV_INDEV_STATE_RELEASED;
        i2c_write(REG_STATUS, 0);
        return;
    }
    int16_t x = pt[1] << 8 | pt[0];
    int16_t y = pt[3] << 8 | pt[2];

    // Clamp - the panel reports outside-of-active-area coordinates
    // when a finger drags off the bezel.
    if (x < 0) x = 0; else if (x >= LCD_H_RES) x = LCD_H_RES - 1;
    if (y < 0) y = 0; else if (y >= LCD_V_RES) y = LCD_V_RES - 1;

    last_x = x;
    last_y = y;
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PRESSED;

    i2c_write(REG_STATUS, 0);
}

} // namespace touch
