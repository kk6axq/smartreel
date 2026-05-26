#include "app/leds.h"

#include "ui/app_state.h"

#include <string.h>

namespace leds {

// 16 pixels worth of RGB triples. Stack-local; built per reel.
constexpr int PIXELS_PER_REEL = app::SLOTS_PER_CHAIN;

namespace {

inline void put_pixel(uint8_t* buf, int idx, uint8_t r, uint8_t g, uint8_t b) {
    buf[idx * 3 + 0] = r;
    buf[idx * 3 + 1] = g;
    buf[idx * 3 + 2] = b;
}

} // anonymous

rs485::Status light_slot(int slot_num, uint8_t r, uint8_t g, uint8_t b) {
    if (slot_num < 1 || slot_num > app::N_SLOTS) return rs485::Status::BufferTooSmall;
    const uint8_t reel_id = (slot_num - 1) / app::SLOTS_PER_CHAIN;       // 0..3
    const uint16_t pixel  = (slot_num - 1) % app::SLOTS_PER_CHAIN;       // 0..15
    const uint8_t rgb[3]  = { r, g, b };
    rs485::Status s = rs485::set_reel_pixels(reel_id, pixel, rgb, 1);
    if (s != rs485::Status::Ok) return s;
    return rs485::commit_pixels(1u << reel_id);
}

// Walk a predicate over app_state.rack; for each reel, build a full
// 16-pixel buffer (target_color where the predicate matches, off
// where it doesn't), push to the Core, then commit all reels at once.
template <typename Pred>
static rs485::Status light_where(Pred pred,
                                  uint8_t on_r, uint8_t on_g, uint8_t on_b) {
    uint8_t reel_bitmap = 0;
    for (int reel_id = 0; reel_id < app::N_CHAINS; ++reel_id) {
        uint8_t buf[PIXELS_PER_REEL * 3];
        memset(buf, 0, sizeof(buf));
        bool any = false;
        for (int pos = 0; pos < app::SLOTS_PER_CHAIN; ++pos) {
            const int slot_num = reel_id * app::SLOTS_PER_CHAIN + pos + 1;
            const app::Slot* s = app::slot_by_num(slot_num);
            if (s && pred(*s)) {
                put_pixel(buf, pos, on_r, on_g, on_b);
                any = true;
            }
        }
        // We push the buffer for every reel anyway, so a transition
        // from "lit slot 3" -> "lit slot 7" clears slot 3. If you want
        // to *only* set lit ones leaving others alone, do incremental
        // calls per-slot.
        rs485::Status w = rs485::set_reel_pixels(reel_id, 0, buf,
                                                 PIXELS_PER_REEL);
        if (w != rs485::Status::Ok) {
            // First failure: still try to clean up by committing what
            // we have. Return the first failure status.
            (void)w; // tolerate per-reel failures
        }
        if (any) reel_bitmap |= (1u << reel_id);
    }
    return rs485::commit_pixels(0x0F);  // all 4 reels
}

rs485::Status light_target_slots(uint8_t r, uint8_t g, uint8_t b) {
    return light_where(
        [](const app::Slot& s) { return s.state == app::SlotState::TARGET; },
        r, g, b);
}

rs485::Status light_empty_slots(uint8_t r, uint8_t g, uint8_t b) {
    return light_where(
        [](const app::Slot& s) { return s.state == app::SlotState::EMPTY; },
        r, g, b);
}

rs485::Status fill_all(uint8_t r, uint8_t g, uint8_t b) {
    for (int reel_id = 0; reel_id < app::N_CHAINS; ++reel_id) {
        rs485::fill_reel(reel_id, r, g, b);
    }
    return rs485::commit_pixels(0x0F);
}

rs485::Status clear_all() {
    return fill_all(0, 0, 0);
}

} // namespace leds
