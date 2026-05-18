#include "display/display.h"
#include "board/board_pins.h"
#include "board/ch422g.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp32-hal-log.h>

namespace display {

static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_disp_drv_t           s_disp_drv;
static lv_disp_draw_buf_t      s_draw_buf;
static lv_disp_t*              s_disp = nullptr;
static lv_color_t*             s_buf1 = nullptr;
static lv_color_t*             s_buf2 = nullptr;

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
    // For an RGB panel "draw" is just a memcpy into the framebuffer.
    // LVGL is configured with two PSRAM buffers (display.cpp:init), so
    // the next refresh has somewhere to draw while the panel rescans
    // the previous frame. That's enough to avoid tearing without a
    // vsync gate.
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px);
    lv_disp_flush_ready(drv);
}

bool init() {
    // 1) Pull LCD out of reset, then turn on backlight LATER (after
    //    LVGL has drawn the first frame, to avoid flashing garbage).
    ch422g::lcd_reset(false);
    delay(10);
    ch422g::lcd_reset(true);
    delay(50);

    // 2) Configure the RGB panel. ESP-IDF 5.x exposes:
    //   - num_fbs = 2          : driver-managed front/back framebuffers
    //   - bounce_buffer_size_px: small SRAM staging buffer that the
    //                            DMA reads from instead of going to
    //                            PSRAM directly. Eliminates the
    //                            scanline-shift artifact on heavy
    //                            PSRAM contention.
    esp_lcd_rgb_panel_config_t panel_cfg = {};
    panel_cfg.clk_src           = LCD_CLK_SRC_DEFAULT;
    panel_cfg.timings.pclk_hz   = LCD_PIXEL_CLOCK_HZ;
    panel_cfg.timings.h_res     = LCD_H_RES;
    panel_cfg.timings.v_res     = LCD_V_RES;
    panel_cfg.timings.hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH;
    panel_cfg.timings.hsync_back_porch  = LCD_HSYNC_BACK_PORCH;
    panel_cfg.timings.hsync_front_porch = LCD_HSYNC_FRONT_PORCH;
    panel_cfg.timings.vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH;
    panel_cfg.timings.vsync_back_porch  = LCD_VSYNC_BACK_PORCH;
    panel_cfg.timings.vsync_front_porch = LCD_VSYNC_FRONT_PORCH;
    panel_cfg.timings.flags.pclk_active_neg = 1;
    panel_cfg.data_width             = 16;
    panel_cfg.bits_per_pixel         = 16;
    panel_cfg.num_fbs                = 2;
    panel_cfg.bounce_buffer_size_px  = LCD_H_RES * 10;   // 10 lines

    panel_cfg.hsync_gpio_num = LCD_PIN_HSYNC;
    panel_cfg.vsync_gpio_num = LCD_PIN_VSYNC;
    panel_cfg.de_gpio_num    = LCD_PIN_DE;
    panel_cfg.pclk_gpio_num  = LCD_PIN_PCLK;
    panel_cfg.disp_gpio_num  = -1;        // not used (controllerless panel)
    panel_cfg.data_gpio_nums[0]  = LCD_PIN_B0;
    panel_cfg.data_gpio_nums[1]  = LCD_PIN_B1;
    panel_cfg.data_gpio_nums[2]  = LCD_PIN_B2;
    panel_cfg.data_gpio_nums[3]  = LCD_PIN_B3;
    panel_cfg.data_gpio_nums[4]  = LCD_PIN_B4;
    panel_cfg.data_gpio_nums[5]  = LCD_PIN_G0;
    panel_cfg.data_gpio_nums[6]  = LCD_PIN_G1;
    panel_cfg.data_gpio_nums[7]  = LCD_PIN_G2;
    panel_cfg.data_gpio_nums[8]  = LCD_PIN_G3;
    panel_cfg.data_gpio_nums[9]  = LCD_PIN_G4;
    panel_cfg.data_gpio_nums[10] = LCD_PIN_G5;
    panel_cfg.data_gpio_nums[11] = LCD_PIN_R0;
    panel_cfg.data_gpio_nums[12] = LCD_PIN_R1;
    panel_cfg.data_gpio_nums[13] = LCD_PIN_R2;
    panel_cfg.data_gpio_nums[14] = LCD_PIN_R3;
    panel_cfg.data_gpio_nums[15] = LCD_PIN_R4;

    panel_cfg.flags.fb_in_psram = true;

    if (esp_lcd_new_rgb_panel(&panel_cfg, &s_panel) != ESP_OK) {
        log_e("esp_lcd_new_rgb_panel failed");
        return false;
    }
    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        log_e("esp_lcd_panel_init failed");
        return false;
    }

    // 3) Allocate two LVGL line buffers in PSRAM.
    //    20 lines is a sweet spot between latency and overhead.
    constexpr size_t buf_lines = 40;
    size_t buf_pixels = LCD_H_RES * buf_lines;
    s_buf1 = static_cast<lv_color_t*>(
        heap_caps_aligned_alloc(64, buf_pixels * sizeof(lv_color_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_buf2 = static_cast<lv_color_t*>(
        heap_caps_aligned_alloc(64, buf_pixels * sizeof(lv_color_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_buf1 || !s_buf2) {
        log_e("LVGL buffer allocation failed");
        return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buf_pixels);

    // 4) Register LVGL display driver.
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.full_refresh = 0;
    s_disp_drv.direct_mode  = 0;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    // 5) Backlight on once we hand control to LVGL.
    ch422g::lcd_backlight(true);
    return true;
}

lv_disp_t* lvgl_display() { return s_disp; }

} // namespace display
