// =====================================================================
//  Reel Rack HMI -- design tokens (light theme, blue accent)
//
//  Mirrors the CSS custom properties in DisplaySkeleton/index.html.
//  Edit those tokens, edit these. Everything else in the UI references
//  these by name so colour drift between mockup + firmware is minimal.
// =====================================================================
#pragma once

#include <lvgl.h>

namespace theme {

// ---- Colour tokens --------------------------------------------------
namespace color {
    // Surfaces
    static inline lv_color_t bg()             { return lv_color_hex(0xFAFAFA); }
    static inline lv_color_t surface()        { return lv_color_hex(0xFFFFFF); }
    static inline lv_color_t border()         { return lv_color_hex(0xE5E5E5); }
    static inline lv_color_t border_strong()  { return lv_color_hex(0xCCCCCC); }
    static inline lv_color_t row_pressed()    { return lv_color_hex(0xF0F0F0); }

    // Text
    static inline lv_color_t text()           { return lv_color_hex(0x1A1A1A); }
    static inline lv_color_t text_muted()     { return lv_color_hex(0x666666); }
    static inline lv_color_t text_on_accent() { return lv_color_hex(0xFFFFFF); }

    // Accent
    static inline lv_color_t accent()         { return lv_color_hex(0x2563EB); }
    static inline lv_color_t accent_pressed() { return lv_color_hex(0x1E40AF); }

    // Slot / state colours
    static inline lv_color_t slot_empty()     { return lv_color_hex(0xE5E5E5); }
    static inline lv_color_t slot_occupied()  { return lv_color_hex(0x404040); }
    static inline lv_color_t slot_target()    { return lv_color_hex(0x2563EB); }
    static inline lv_color_t slot_picked()    { return lv_color_hex(0x16A34A); }
    static inline lv_color_t slot_error()     { return lv_color_hex(0xDC2626); }
    static inline lv_color_t slot_warn()      { return lv_color_hex(0xF59E0B); }

    // Modal-backdrop dim (rgba(0,0,0,0.4))
    static inline lv_color_t backdrop()       { return lv_color_black(); }
    static constexpr lv_opa_t backdrop_opa = LV_OPA_40;
}

// ---- Layout constants ----------------------------------------------
namespace layout {
    static constexpr int SCREEN_W   = 800;
    static constexpr int SCREEN_H   = 480;
    static constexpr int STATUS_H   = 64;   // sized for the 28pt title
    static constexpr int BODY_H     = SCREEN_H - STATUS_H;
    static constexpr int RADIUS     = 6;
    static constexpr int PAD_EDGE   = 10;
    static constexpr int CARD_PAD   = 12;
    static constexpr int CARD_GAP   = 8;
}

// ---- Pre-built reusable styles -------------------------------------
struct Styles {
    // Screen background
    lv_style_t screen_bg;

    // Status bar
    lv_style_t statusbar;
    lv_style_t statusbar_back_btn;
    lv_style_t statusbar_pill;
    lv_style_t statusbar_badge;

    // Cards / panels
    lv_style_t card;
    lv_style_t card_pad_tight;        // 8/10 instead of 12

    // Tile (home menu, configure menu)
    lv_style_t tile;
    lv_style_t tile_pressed;

    // Row (lists)
    lv_style_t row;
    lv_style_t row_stripe_empty;
    lv_style_t row_stripe_occupied;
    lv_style_t row_stripe_target;
    lv_style_t row_stripe_picked;
    lv_style_t row_stripe_error;
    lv_style_t row_stripe_accent;

    // Buttons
    lv_style_t btn;                   // default outlined
    lv_style_t btn_primary;
    lv_style_t btn_success;
    lv_style_t btn_danger;
    lv_style_t btn_disabled;
    lv_style_t row_btn;               // smaller, used inside list rows
    lv_style_t row_btn_muted;
    lv_style_t row_btn_success;

    // Inputs (we don't actually take input, but render shapes that
    // look like inputs so the form pages match the mockup).
    lv_style_t input;
    lv_style_t toggle_track;
    lv_style_t toggle_track_on;
    lv_style_t toggle_thumb;

    // Form
    lv_style_t form_card;
    lv_style_t form_card_head;
    lv_style_t form_row;

    // Dot grid
    lv_style_t dot_empty;
    lv_style_t dot_occupied;
    lv_style_t dot_target;
    lv_style_t dot_picked;
    lv_style_t dot_error;
    lv_style_t dot_warn;

    // Modal
    lv_style_t modal_box;
    lv_style_t modal_box_warn;
    lv_style_t modal_box_info;
    lv_style_t modal_header_error;
    lv_style_t modal_header_warn;
    lv_style_t modal_header_info;

    // Progress bar (pick screen)
    lv_style_t progress_bg;
    lv_style_t progress_fill;
};

const Styles& s();
void init();

} // namespace theme
