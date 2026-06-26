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
#include <freertos/task.h>

#include <string.h>

namespace display {

// ---------------------------------------------------------------------
//  Tearing-free strategy: Waveshare AVOID_TEARING_MODE 3
//      LCD double-buffer (num_fbs=2)
//      + LVGL direct_mode (LVGL writes the dirty rect into the active
//        FB; the FB IS the LVGL draw buffer, no copy through a small
//        bounce-style LVGL buffer)
//      + vsync-gated swap (flush_cb requests a swap and blocks until
//        the panel actually flips at the next vsync)
//      + dirty-area copy from front -> back after each swap so the
//        FB LVGL is about to write into next has the latest pixels
//        for unchanged regions
//
//  Why this beats AVOID_TEARING_MODE 1 (full_refresh):
//      Mode 1 re-renders the whole screen every time anything changes.
//      A once-per-second status-label update therefore costs a full
//      768 KB write to PSRAM, which can starve the LCD DMA bounce
//      buffer (visible as bandwidth tearing). Mode 3 only re-renders
//      the actually-dirty rect, so tiny periodic updates are nearly
//      free.
//
//  Why this is more complex than mode 1:
//      LVGL's "dirty rect" only describes what LVGL just drew into the
//      currently-active FB. The OTHER FB doesn't have that drawing.
//      So after we swap to show the just-drawn FB, we have to copy
//      the dirty regions from the new front -> the new back, otherwise
//      LVGL's next render (which only touches dirty pixels) would
//      compose against stale background.
// ---------------------------------------------------------------------

static esp_lcd_panel_handle_t s_panel    = nullptr;
static lv_disp_drv_t           s_disp_drv;
static lv_disp_draw_buf_t      s_draw_buf;
static lv_disp_t*              s_disp    = nullptr;
static SemaphoreHandle_t       s_vsync_sem = nullptr;
static void*                   s_fb0     = nullptr;
static void*                   s_fb1     = nullptr;

// One LVGL refresh can produce up to LV_INV_BUF_SIZE separate dirty
// rects; flush_cb is called once per rect. We accumulate them in
// s_dirty[] across the refresh, then act on the whole batch on the
// last call (detected via lv_disp_flush_is_last).
static constexpr int MAX_DIRTY = LV_INV_BUF_SIZE;
static lv_area_t  s_dirty[MAX_DIRTY];
static int        s_n_dirty = 0;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t /*panel*/,
                               const esp_lcd_rgb_panel_event_data_t* /*edata*/,
                               void* /*user_ctx*/) {
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync_sem, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

// Copy one rectangular region from src FB to dst FB. Both FBs are
// LCD_H_RES wide; the rect coordinates are in pixels. Each row is
// memcpy'd directly out of PSRAM into PSRAM.
//
// A.3 (deferred review item R3): this copy reads the just-swapped FRONT
// buffer while the LCD DMA is simultaneously scanning that same buffer
// out of PSRAM. A full-screen blit (~768 KB on a page change) spikes
// PSRAM-bus demand for several milliseconds and starves the LCD bounce
// buffer -> a visible horizontal tear. So for tall rects we split the
// blit into row-bands and yield briefly between them, letting the DMA
// refill the bounce buffer in the gaps. Small dirty rects (the common
// case: a status label, a button press) stay under the threshold and
// copy in one shot, so periodic updates remain cheap.
static constexpr int COPY_BAND_ROWS = 48;
static void copy_rect(void* dst, const void* src, const lv_area_t& a) {
    const int x      = a.x1;
    const int width  = (a.x2 - a.x1 + 1);
    const int rows   = (a.y2 - a.y1 + 1);
    const size_t row_bytes = (size_t)width * sizeof(lv_color_t);
    const bool chunked = rows > COPY_BAND_ROWS;
    for (int y = 0; y < rows; ++y) {
        const lv_color_t* s = static_cast<const lv_color_t*>(src) +
                              (a.y1 + y) * LCD_H_RES + x;
        lv_color_t*       d = static_cast<lv_color_t*>(dst) +
                              (a.y1 + y) * LCD_H_RES + x;
        memcpy(d, s, row_bytes);
        if (chunked && (y % COPY_BAND_ROWS) == (COPY_BAND_ROWS - 1))
            vTaskDelay(1);    // ~1 ms: let the LCD DMA refill its bounce buffer
    }
}

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    // Stash this dirty rect so we can sync the other FB after the
    // swap. LVGL caps the total at LV_INV_BUF_SIZE; we guard anyway.
    if (s_n_dirty < MAX_DIRTY) {
        s_dirty[s_n_dirty++] = *area;
    }

    // If more rects are coming for this refresh, just acknowledge and
    // wait for the last one.
    if (!lv_disp_flush_is_last(drv)) {
        lv_disp_flush_ready(drv);
        return;
    }

    // Last flush of this refresh: schedule the swap to the FB LVGL
    // just rendered into. `color_map` is either s_fb0 or s_fb1; the
    // IDF driver recognises the pointer and treats this as a back-
    // buffer swap (no copy).
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, color_map);

    // Block until the swap actually happens at the next vsync.
    xSemaphoreTake(s_vsync_sem, portMAX_DELAY);

    // After the swap, color_map IS the front (visible) FB; the OTHER
    // one is the new back. Copy this refresh's dirty rects from front
    // -> back so LVGL's next render starts from in-sync content.
    void* new_back = (color_map == static_cast<lv_color_t*>(s_fb0)) ? s_fb1 : s_fb0;
    for (int i = 0; i < s_n_dirty; ++i) {
        copy_rect(new_back, color_map, s_dirty[i]);
    }
    s_n_dirty = 0;

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
    panel_cfg.num_fbs                = 2;
    // A.1 (deferred review item R3): the LCD DMA stages scanlines through
    // this bounce buffer out of the PSRAM framebuffer. A larger bounce
    // buffer tolerates longer PSRAM-bus stalls -- e.g. while flush_cb's
    // dirty-rect copy is contending for PSRAM bandwidth -- before it
    // underflows into a visible tear. 20 scanlines (was 10) ~= 64 KB of
    // internal DMA RAM total across the two buffers; raise further if
    // tearing persists and internal RAM allows.
    panel_cfg.bounce_buffer_size_px  = LCD_H_RES * 20;

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
    //    LVGL directly as its draw buffers.
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &s_fb0, &s_fb1) != ESP_OK
        || !s_fb0 || !s_fb1) {
        log_e("esp_lcd_rgb_panel_get_frame_buffer failed");
        return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf,
                          static_cast<lv_color_t*>(s_fb0),
                          static_cast<lv_color_t*>(s_fb1),
                          LCD_H_RES * LCD_V_RES);

    // 5) Register LVGL driver in direct_mode. LVGL will write each
    //    dirty rect directly into the active FB (one of s_fb0/s_fb1);
    //    flush_cb accumulates the rects, then on the last flush of
    //    the refresh swaps + waits for vsync + syncs the dirty rects
    //    over to the new back FB. No full-screen renders on tiny
    //    updates.
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = LCD_H_RES;
    s_disp_drv.ver_res      = LCD_V_RES;
    s_disp_drv.flush_cb     = flush_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.full_refresh = 0;
    s_disp_drv.direct_mode  = 1;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    // 6) Backlight on once we hand control to LVGL.
    ch422g::lcd_backlight(true);
    return true;
}

lv_disp_t* lvgl_display() { return s_disp; }

} // namespace display
