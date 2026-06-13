#include "app/leds.h"

#include "ui/app_state.h"
#include "app/hw_mirror.h"

#include <Arduino.h>
#include <string.h>

namespace leds {

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

// Light a logical slot across its FULL width -- every physical reel-slot
// in the combined run. A standard slot is one pixel; a slot built from a
// 4-wide combine lights all four pixels it spans.
rs485::Status light_slot(int slot_num, uint8_t r, uint8_t g, uint8_t b) {
    const app::Slot* s = app::slot_by_num(slot_num);
    if (!s) return rs485::Status::BufferTooSmall;
    int w = s->width > 0 ? s->width : 1;
    if (w > MAX_PIX) w = MAX_PIX;
    uint8_t rgb[MAX_PIX * 3];
    for (int i = 0; i < w; ++i) put_pixel(rgb, i, r, g, b);
    const uint16_t start = (uint16_t)(s->module * 16 + s->mslot);
    rs485::Status st = rs485::set_reel_pixels(s->port, start, rgb, (uint16_t)w);
    if (st != rs485::Status::Ok) return st;
    return rs485::commit_pixels(1u << s->port);
}

// Build a full per-port pixel buffer lighting every logical slot that
// matches `pred`, push it, then commit all touched ports atomically.
template <typename Pred>
static rs485::Status light_where(Pred pred,
                                 uint8_t on_r, uint8_t on_g, uint8_t on_b) {
    app::State& st = app::state();
    uint8_t reel_bitmap = 0;
    for (int p = 0; p < app::N_PORTS; ++p) {
        const int npx = port_pixels(p);
        if (npx <= 0) continue;
        uint8_t buf[MAX_PIX * 3];
        memset(buf, 0, sizeof(buf));
        for (int i = 0; i < st.n_rack; ++i) {
            const app::Slot& s = st.rack[i];
            if (s.port != p || !pred(s)) continue;
            // Light the slot's full combined span so a wide logical slot
            // lights every physical pixel it occupies.
            const int start = s.module * 16 + s.mslot;
            const int w     = s.width > 0 ? s.width : 1;
            for (int k = 0; k < w; ++k) {
                const int px = start + k;
                if (px >= 0 && px < npx) put_pixel(buf, px, on_r, on_g, on_b);
            }
        }
        rs485::set_reel_pixels(p, 0, buf, npx);   // tolerate per-port failures
        reel_bitmap |= (1u << p);
    }
    if (!reel_bitmap) return rs485::Status::Ok;
    return rs485::commit_pixels(reel_bitmap);
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
    uint8_t reel_bitmap = 0;
    for (int p = 0; p < app::N_PORTS; ++p) {
        if (port_pixels(p) <= 0) continue;
        rs485::fill_reel(p, r, g, b);
        reel_bitmap |= (1u << p);
    }
    if (!reel_bitmap) return rs485::Status::Ok;
    return rs485::commit_pixels(reel_bitmap);
}

rs485::Status clear_all() { return fill_all(0, 0, 0); }

void boot_chase() {
    constexpr int      STEP_MS = 35;             // dwell per pixel
    constexpr uint8_t  R = 0x25, G = 0x63, B = 0xEB;   // theme blue

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
    clear_all();   // end dark regardless of what was on at power-up
}

} // namespace leds
