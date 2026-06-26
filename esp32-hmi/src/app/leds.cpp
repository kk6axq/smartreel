#include "app/leds.h"

#include "ui/app_state.h"
#include "app/hw_mirror.h"

#include <Arduino.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace leds {

// =====================================================================
//  Async LED engine (review item B: keep RS485 off the LVGL thread).
//
//  Every leds:: call used to push pixels + commit synchronously over
//  RS485 -- a multi-millisecond blocking bus round-trip. Callers run on
//  the LVGL render thread (the 500 ms alarm-flash tick, slot-change
//  handlers, pick/load handlers), so each call stalled lv_timer_handler
//  and added to display jank.
//
//  Now the public API only mutates an in-RAM shadow of the desired LED
//  state (cheap, on the caller's thread) and signals a worker. A single
//  worker task pinned to APP_CPU (never PRO_CPU, which runs the RGB LCD
//  DMA -- see memory hmi-cpu-pinning) drains the shadow to the Core over
//  RS485. Rapid updates (e.g. the alarm flash) coalesce: the worker
//  always pushes the latest shadow, never a backlog.
// =====================================================================

// Max LED pixels on one port's strip (4 modules x 16 px).
static constexpr int MAX_PIX = app::N_PORTS == 0 ? 64 : 4 * 16;

namespace {
inline void put_pixel(uint8_t* buf, int idx, uint8_t r, uint8_t g, uint8_t b) {
    buf[idx * 3 + 0] = r;
    buf[idx * 3 + 1] = g;
    buf[idx * 3 + 2] = b;
}
// Pixel count on a port's strip = module_count * 16.
inline int port_pixels(int port) { return hw_mirror::module_count(port) * 16; }
} // anonymous

// ---- Shadow state shared between the caller threads and the worker ----
// s_shadow holds the desired RGB for every pixel on every port; s_dirty
// marks which ports changed since the last push. Both are guarded by
// s_mtx; s_sig wakes the worker.
static uint8_t           s_shadow[app::N_PORTS][MAX_PIX * 3];
static uint8_t           s_dirty   = 0;
static SemaphoreHandle_t s_mtx     = nullptr;
static SemaphoreHandle_t s_sig     = nullptr;
static TaskHandle_t      s_worker  = nullptr;

// Mark ports dirty (under the lock the caller already holds) is folded
// into each setter; this just signals the worker after the lock drops.
static inline void wake_worker() {
    if (s_sig) xSemaphoreGive(s_sig);
}

static void led_worker(void* /*arg*/) {
    for (;;) {
        xSemaphoreTake(s_sig, portMAX_DELAY);

        // Drain until no port is dirty. Snapshot under the lock (cheap,
        // <1 KB) then push outside it, so caller-thread setters never
        // block on the RS485 round-trip.
        for (;;) {
            uint8_t dirty = 0;
            static uint8_t snap[app::N_PORTS][MAX_PIX * 3];
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            dirty = s_dirty;
            s_dirty = 0;
            if (dirty) memcpy(snap, s_shadow, sizeof(snap));
            xSemaphoreGive(s_mtx);
            if (!dirty) break;

            uint8_t bitmap = 0;
            for (int p = 0; p < app::N_PORTS; ++p) {
                if (!(dirty & (1u << p))) continue;
                const int npx = port_pixels(p);
                if (npx <= 0) continue;
                rs485::set_reel_pixels((uint8_t)p, 0, snap[p], (uint16_t)npx);
                bitmap |= (1u << p);
            }
            if (bitmap) rs485::commit_pixels(bitmap);
        }
    }
}

void start() {
    if (s_worker) return;                       // idempotent
    s_mtx = xSemaphoreCreateMutex();
    s_sig = xSemaphoreCreateBinary();
    if (!s_mtx || !s_sig) return;
    // APP_CPU, priority 1 (below the LVGL task at 2). 4 KB stack covers
    // the RS485 transact call frames (the shadow snapshot is static).
    xTaskCreatePinnedToCore(led_worker, "led", 4096, nullptr, 1, &s_worker,
                            APP_CPU_NUM);
}

// ---- Public API: mutate the shadow, mark dirty, wake the worker -------

rs485::Status light_slot(int slot_num, uint8_t r, uint8_t g, uint8_t b) {
    const app::Slot* s = app::slot_by_num(slot_num);
    if (!s) return rs485::Status::BufferTooSmall;   // invalid slot (review item R16)
    if (!s_mtx) return rs485::Status::Ok;           // engine not up yet (pre-start)
    int w = s->width > 0 ? s->width : 1;
    const int start = s->module * 16 + s->mslot;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < w; ++i) {
        const int px = start + i;
        if (px >= 0 && px < MAX_PIX) put_pixel(s_shadow[s->port], px, r, g, b);
    }
    s_dirty |= (1u << s->port);
    xSemaphoreGive(s_mtx);
    wake_worker();
    return rs485::Status::Ok;
}

// Build the shadow for every port from a slot predicate: every matching
// logical slot lights its full combined span, everything else goes dark.
template <typename Pred>
static rs485::Status light_where(Pred pred,
                                 uint8_t on_r, uint8_t on_g, uint8_t on_b) {
    if (!s_mtx) return rs485::Status::Ok;
    app::State& st = app::state();
    uint8_t ports = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int p = 0; p < app::N_PORTS; ++p) {
        const int npx = port_pixels(p);
        if (npx <= 0) continue;
        memset(s_shadow[p], 0, MAX_PIX * 3);
        for (int i = 0; i < st.n_rack; ++i) {
            const app::Slot& s = st.rack[i];
            if (s.port != p || !pred(s)) continue;
            const int start = s.module * 16 + s.mslot;
            const int w     = s.width > 0 ? s.width : 1;
            for (int k = 0; k < w; ++k) {
                const int px = start + k;
                if (px >= 0 && px < npx) put_pixel(s_shadow[p], px, on_r, on_g, on_b);
            }
        }
        ports |= (1u << p);
    }
    s_dirty |= ports;
    xSemaphoreGive(s_mtx);
    if (ports) wake_worker();
    return rs485::Status::Ok;
}

rs485::Status light_target_slots(uint8_t r, uint8_t g, uint8_t b) {
    return light_where(
        [](const app::Slot& s) { return s.state == app::SlotState::TARGET; }, r, g, b);
}

rs485::Status light_empty_slots(uint8_t r, uint8_t g, uint8_t b) {
    return light_where(
        [](const app::Slot& s) { return s.state == app::SlotState::EMPTY; }, r, g, b);
}

rs485::Status fill_all(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_mtx) return rs485::Status::Ok;
    uint8_t ports = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int p = 0; p < app::N_PORTS; ++p) {
        const int npx = port_pixels(p);
        if (npx <= 0) continue;
        for (int i = 0; i < npx && i < MAX_PIX; ++i) put_pixel(s_shadow[p], i, r, g, b);
        ports |= (1u << p);
    }
    s_dirty |= ports;
    xSemaphoreGive(s_mtx);
    if (ports) wake_worker();
    return rs485::Status::Ok;
}

rs485::Status clear_all() { return fill_all(0, 0, 0); }

void boot_chase() {
    constexpr int      STEP_MS = 35;             // dwell per pixel
    constexpr uint8_t  R = 0x25, G = 0x63, B = 0xEB;   // theme blue

    // Runs on the setup thread before the worker/LVGL exist, so it talks
    // to the bus directly (blocking is fine here). It leaves every LED off,
    // matching the zero-initialised shadow the worker starts from.
    for (int p = 0; p < app::N_PORTS; ++p) {
        const int npx = port_pixels(p);
        if (npx <= 0) continue;                  // no modules on this port
        uint8_t buf[MAX_PIX * 3];
        for (int i = 0; i < npx && i < MAX_PIX; ++i) {
            memset(buf, 0, (size_t)npx * 3);     // only this pixel lit
            put_pixel(buf, i, R, G, B);
            rs485::set_reel_pixels(p, 0, buf, (uint16_t)npx);
            rs485::commit_pixels(1u << p);
            delay(STEP_MS);
        }
    }
    // End dark, synchronously (the async clear_all path isn't running yet).
    for (int p = 0; p < app::N_PORTS; ++p) {
        if (port_pixels(p) <= 0) continue;
        rs485::fill_reel(p, 0, 0, 0);
        rs485::commit_pixels(1u << p);
    }
}

} // namespace leds
