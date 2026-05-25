#include "display/display.h"
#include "board/board_pins.h"
#include "board/ch422g.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp32-hal-log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace display {

// ---------------------------------------------------------------------
//  Tearing-free strategy (matches Waveshare ESP32-S3-Touch-LCD-4.3B
//  reference, AVOID_TEARING_MODE = 1: LCD double-buffer + LVGL
//  full-refresh).
//
//  - The IDF RGB panel driver owns two framebuffers in PSRAM (num_fbs=2).
//  - We hand those FB pointers to LVGL as its two draw buffers (no
//    extra PSRAM allocations, full-screen each).
//  - LVGL runs in `full_refresh` mode: it redraws every frame from
//    scratch into the currently-active draw buffer, then calls
//    flush_cb once per frame with the entire screen rect.
//  - flush_cb calls esp_lcd_panel_draw_bitmap for the whole screen.
//    Because the buffer pointer matches one of the driver's known
//    FBs, the driver just schedules the swap for the next vsync
//    instead of copying.
//  - The on_vsync ISR callback signals a binary semaphore; flush_cb
//    blocks on it until the swap commits, then returns LVGL flush
//    ready. The just-departed FB becomes LVGL's next draw target.
//
//  Result: LVGL never partially-updates a FB that's being scanned
//  out. Scrolling lists, list (re)builds, anomaly-modal pop-ups all
//  draw cleanly.
// ---------------------------------------------------------------------

static esp_lcd_panel_handle_t s_panel    = nullptr;
static lv_disp_drv_t           s_disp_drv;
static lv_disp_draw_buf_t      s_draw_buf;
static lv_disp_t*              s_disp    = nullptr;
static SemaphoreHandle_t       s_vsync_sem = nullptr;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t /*panel*/,
                               const esp_lcd_rgb_panel_event_data_t* /*edata*/,
                               void* /*user_ctx*/) {
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync_sem, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* /*area*/, lv_color_t* px) {
    // LVGL is in full_refresh mode and `px` points at one of the two
    // panel framebuffers (whichever LVGL just rendered into). The IDF
    // driver detects that pointer match and treats this as a back-buf
    // swap rather than a copy.
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, px);

    // Wait for the panel to actually swap (one vsync). Until that
    // fires, the FB LVGL just wrote is being scanned out -- LVGL must
    // not reuse it.
    xSemaphoreTake(s_vsync_sem, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}

bool init() {
    // 1) Pull LCD out of reset. Backlight stays off until after the
    //    first LVGL flush to avoid flashing garbage.
    ch422g::lcd_reset(false);
    delay(10);
    ch422g::lcd_reset(true);
    delay(50);

    // 2) Configure the RGB panel.
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
    panel_cfg.num_fbs                = 2;                  // front + back
    panel_cfg.bounce_buffer_size_px  = LCD_H_RES * 10;     // 10 lines

    panel_cfg.hsync_gpio_num = LCD_PIN_HSYNC;
    panel_cfg.vsync_gpio_num = LCD_PIN_VSYNC;
    panel_cfg.de_gpio_num    = LCD_PIN_DE;
    panel_cfg.pclk_gpio_num  = LCD_PIN_PCLK;
    panel_cfg.disp_gpio_num  = -1;
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

    // 3) Register the vsync callback BEFORE init so it's armed by
    //    the time the first frame fires.
    s_vsync_sem = xSemaphoreCreateBinary();
    if (!s_vsync_sem) {
        log_e("vsync semaphore alloc failed");
        return false;
    }
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_vsync = on_vsync;
    esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr);

    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        log_e("esp_lcd_panel_init failed");
        return false;
    }

    // 4) Pull the two driver-owned framebuffer pointers out of the
    //    panel. These are full-screen PSRAM buffers we can hand to
    //    LVGL directly -- no extra allocation needed.
    void* fb0 = nullptr;
    void* fb1 = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1) != ESP_OK
        || !fb0 || !fb1) {
        log_e("esp_lcd_rgb_panel_get_frame_buffer failed");
        return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf,
                          static_cast<lv_color_t*>(fb0),
                          static_cast<lv_color_t*>(fb1),
                          LCD_H_RES * LCD_V_RES);

    // 5) Register LVGL display driver in full_refresh + vsync-gated
    //    mode. full_refresh tells LVGL "always redraw the whole
    //    screen into the supplied buffer"; combined with our FB-
    //    backed draw_buf this means each frame is rendered to the
    //    inactive FB and swapped on vsync.
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = LCD_H_RES;
    s_disp_drv.ver_res      = LCD_V_RES;
    s_disp_drv.flush_cb     = flush_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.full_refresh = 1;
    s_disp_drv.direct_mode  = 0;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    // 6) Backlight on once we hand control to LVGL.
    ch422g::lcd_backlight(true);
    return true;
}

lv_disp_t* lvgl_display() { return s_disp; }

} // namespace display
