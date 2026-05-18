#include "ui/theme.h"

namespace theme {

static Styles g_s = {};
static bool   g_inited = false;

const Styles& s() { return g_s; }

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
static void make_card(lv_style_t* st, int pad = layout::CARD_PAD) {
    lv_style_init(st);
    lv_style_set_bg_color(st, color::surface());
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_border_color(st, color::border());
    lv_style_set_border_width(st, 1);
    lv_style_set_radius(st, layout::RADIUS);
    lv_style_set_pad_all(st, pad);
    lv_style_set_pad_gap(st, layout::CARD_GAP);
}

static void make_btn_base(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_bg_color(st, color::surface());
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_border_color(st, color::border_strong());
    lv_style_set_border_width(st, 1);
    lv_style_set_radius(st, layout::RADIUS);
    lv_style_set_text_color(st, color::text());
    lv_style_set_text_font(st, &lv_font_montserrat_14);
    lv_style_set_pad_hor(st, 16);
    lv_style_set_pad_ver(st, 8);
    lv_style_set_min_height(st, 36);
}

static void make_dot(lv_style_t* st, lv_color_t c) {
    lv_style_init(st);
    lv_style_set_bg_color(st, c);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_border_width(st, 0);
    lv_style_set_radius(st, 2);
    lv_style_set_pad_all(st, 0);
}

void init() {
    if (g_inited) return;
    g_inited = true;

    // Screen background ----------------------------------------------
    lv_style_init(&g_s.screen_bg);
    lv_style_set_bg_color(&g_s.screen_bg, color::bg());
    lv_style_set_bg_opa(&g_s.screen_bg, LV_OPA_COVER);
    lv_style_set_text_font(&g_s.screen_bg, &lv_font_montserrat_14);
    lv_style_set_text_color(&g_s.screen_bg, color::text());
    lv_style_set_pad_all(&g_s.screen_bg, 0);
    lv_style_set_border_width(&g_s.screen_bg, 0);

    // Status bar -----------------------------------------------------
    lv_style_init(&g_s.statusbar);
    lv_style_set_bg_color(&g_s.statusbar, color::surface());
    lv_style_set_bg_opa(&g_s.statusbar, LV_OPA_COVER);
    lv_style_set_border_side(&g_s.statusbar, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&g_s.statusbar, color::border());
    lv_style_set_border_width(&g_s.statusbar, 1);
    lv_style_set_pad_hor(&g_s.statusbar, 10);
    lv_style_set_pad_ver(&g_s.statusbar, 0);
    lv_style_set_pad_gap(&g_s.statusbar, 10);
    lv_style_set_radius(&g_s.statusbar, 0);

    lv_style_init(&g_s.statusbar_back_btn);
    lv_style_set_bg_color(&g_s.statusbar_back_btn, color::surface());
    lv_style_set_bg_opa(&g_s.statusbar_back_btn, LV_OPA_COVER);
    lv_style_set_border_color(&g_s.statusbar_back_btn, color::border_strong());
    lv_style_set_border_width(&g_s.statusbar_back_btn, 1);
    lv_style_set_radius(&g_s.statusbar_back_btn, layout::RADIUS);
    lv_style_set_text_color(&g_s.statusbar_back_btn, color::text());
    lv_style_set_text_font(&g_s.statusbar_back_btn, &lv_font_montserrat_18);
    lv_style_set_pad_all(&g_s.statusbar_back_btn, 0);

    lv_style_init(&g_s.statusbar_pill);
    lv_style_set_bg_color(&g_s.statusbar_pill, color::surface());
    lv_style_set_bg_opa(&g_s.statusbar_pill, LV_OPA_COVER);
    lv_style_set_border_color(&g_s.statusbar_pill, color::border());
    lv_style_set_border_width(&g_s.statusbar_pill, 1);
    lv_style_set_radius(&g_s.statusbar_pill, 10);
    lv_style_set_pad_hor(&g_s.statusbar_pill, 8);
    lv_style_set_pad_ver(&g_s.statusbar_pill, 3);
    lv_style_set_pad_gap(&g_s.statusbar_pill, 4);
    lv_style_set_text_color(&g_s.statusbar_pill, color::text_muted());
    lv_style_set_text_font(&g_s.statusbar_pill, &lv_font_montserrat_12);

    lv_style_init(&g_s.statusbar_badge);
    lv_style_set_bg_color(&g_s.statusbar_badge, color::slot_error());
    lv_style_set_bg_opa(&g_s.statusbar_badge, LV_OPA_COVER);
    lv_style_set_radius(&g_s.statusbar_badge, 10);
    lv_style_set_pad_hor(&g_s.statusbar_badge, 7);
    lv_style_set_pad_ver(&g_s.statusbar_badge, 2);
    lv_style_set_text_color(&g_s.statusbar_badge, color::text_on_accent());
    lv_style_set_text_font(&g_s.statusbar_badge, &lv_font_montserrat_12);
    lv_style_set_border_width(&g_s.statusbar_badge, 0);

    // Cards ----------------------------------------------------------
    make_card(&g_s.card);
    make_card(&g_s.card_pad_tight, 10);

    // Tile -----------------------------------------------------------
    make_card(&g_s.tile, 14);
    lv_style_init(&g_s.tile_pressed);
    lv_style_set_bg_color(&g_s.tile_pressed, color::row_pressed());

    // Row ------------------------------------------------------------
    lv_style_init(&g_s.row);
    lv_style_set_bg_color(&g_s.row, color::surface());
    lv_style_set_bg_opa(&g_s.row, LV_OPA_COVER);
    lv_style_set_border_color(&g_s.row, color::border());
    lv_style_set_border_width(&g_s.row, 1);
    lv_style_set_radius(&g_s.row, layout::RADIUS);
    lv_style_set_pad_hor(&g_s.row, 10);
    lv_style_set_pad_ver(&g_s.row, 8);
    lv_style_set_pad_gap(&g_s.row, 10);
    lv_style_set_min_height(&g_s.row, 44);

    auto stripe_color = [](lv_style_t* st, lv_color_t c) {
        lv_style_init(st);
        lv_style_set_bg_color(st, c);
        lv_style_set_bg_opa(st, LV_OPA_COVER);
        lv_style_set_radius(st, 2);
        lv_style_set_border_width(st, 0);
    };
    stripe_color(&g_s.row_stripe_empty,    color::slot_empty());
    stripe_color(&g_s.row_stripe_occupied, color::slot_occupied());
    stripe_color(&g_s.row_stripe_target,   color::slot_target());
    stripe_color(&g_s.row_stripe_picked,   color::slot_picked());
    stripe_color(&g_s.row_stripe_error,    color::slot_error());
    stripe_color(&g_s.row_stripe_accent,   color::accent());

    // Buttons --------------------------------------------------------
    make_btn_base(&g_s.btn);

    lv_style_init(&g_s.btn_primary);
    lv_style_set_bg_color(&g_s.btn_primary, color::accent());
    lv_style_set_border_color(&g_s.btn_primary, color::accent());
    lv_style_set_text_color(&g_s.btn_primary, color::text_on_accent());

    lv_style_init(&g_s.btn_success);
    lv_style_set_bg_color(&g_s.btn_success, color::slot_picked());
    lv_style_set_border_color(&g_s.btn_success, color::slot_picked());
    lv_style_set_text_color(&g_s.btn_success, color::text_on_accent());

    lv_style_init(&g_s.btn_danger);
    lv_style_set_bg_color(&g_s.btn_danger, color::slot_error());
    lv_style_set_border_color(&g_s.btn_danger, color::slot_error());
    lv_style_set_text_color(&g_s.btn_danger, color::text_on_accent());

    lv_style_init(&g_s.btn_disabled);
    lv_style_set_bg_color(&g_s.btn_disabled, color::border());
    lv_style_set_text_color(&g_s.btn_disabled, color::text_muted());
    lv_style_set_border_color(&g_s.btn_disabled, color::border());

    // Row-internal compact button
    lv_style_init(&g_s.row_btn);
    lv_style_set_bg_color(&g_s.row_btn, color::accent());
    lv_style_set_bg_opa(&g_s.row_btn, LV_OPA_COVER);
    lv_style_set_border_width(&g_s.row_btn, 0);
    lv_style_set_radius(&g_s.row_btn, layout::RADIUS);
    lv_style_set_text_color(&g_s.row_btn, color::text_on_accent());
    lv_style_set_text_font(&g_s.row_btn, &lv_font_montserrat_12);
    lv_style_set_pad_hor(&g_s.row_btn, 12);
    lv_style_set_pad_ver(&g_s.row_btn, 7);

    lv_style_init(&g_s.row_btn_muted);
    lv_style_set_bg_color(&g_s.row_btn_muted, color::surface());
    lv_style_set_text_color(&g_s.row_btn_muted, color::text());
    lv_style_set_border_color(&g_s.row_btn_muted, color::border_strong());
    lv_style_set_border_width(&g_s.row_btn_muted, 1);

    lv_style_init(&g_s.row_btn_success);
    lv_style_set_bg_color(&g_s.row_btn_success, color::slot_picked());
    lv_style_set_text_color(&g_s.row_btn_success, color::text_on_accent());

    // Input (read-only-looking) --------------------------------------
    lv_style_init(&g_s.input);
    lv_style_set_bg_color(&g_s.input, color::bg());
    lv_style_set_bg_opa(&g_s.input, LV_OPA_COVER);
    lv_style_set_border_color(&g_s.input, color::border_strong());
    lv_style_set_border_width(&g_s.input, 1);
    lv_style_set_radius(&g_s.input, layout::RADIUS);
    lv_style_set_text_color(&g_s.input, color::text());
    lv_style_set_text_font(&g_s.input, &lv_font_montserrat_14);
    lv_style_set_pad_hor(&g_s.input, 8);
    lv_style_set_pad_ver(&g_s.input, 6);

    // Toggle ---------------------------------------------------------
    lv_style_init(&g_s.toggle_track);
    lv_style_set_bg_color(&g_s.toggle_track, color::border());
    lv_style_set_bg_opa(&g_s.toggle_track, LV_OPA_COVER);
    lv_style_set_radius(&g_s.toggle_track, 11);
    lv_style_set_border_width(&g_s.toggle_track, 0);

    lv_style_init(&g_s.toggle_track_on);
    lv_style_set_bg_color(&g_s.toggle_track_on, color::accent());

    lv_style_init(&g_s.toggle_thumb);
    lv_style_set_bg_color(&g_s.toggle_thumb, color::surface());
    lv_style_set_bg_opa(&g_s.toggle_thumb, LV_OPA_COVER);
    lv_style_set_radius(&g_s.toggle_thumb, LV_RADIUS_CIRCLE);
    lv_style_set_border_color(&g_s.toggle_thumb, color::border_strong());
    lv_style_set_border_width(&g_s.toggle_thumb, 1);

    // Form -----------------------------------------------------------
    make_card(&g_s.form_card, 12);

    lv_style_init(&g_s.form_card_head);
    lv_style_set_bg_opa(&g_s.form_card_head, LV_OPA_TRANSP);
    lv_style_set_text_color(&g_s.form_card_head, color::text_muted());
    lv_style_set_text_font(&g_s.form_card_head, &lv_font_montserrat_12);
    lv_style_set_border_width(&g_s.form_card_head, 0);
    lv_style_set_pad_all(&g_s.form_card_head, 0);

    lv_style_init(&g_s.form_row);
    lv_style_set_bg_opa(&g_s.form_row, LV_OPA_TRANSP);
    lv_style_set_border_side(&g_s.form_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&g_s.form_row, color::border());
    lv_style_set_border_width(&g_s.form_row, 1);
    lv_style_set_radius(&g_s.form_row, 0);
    lv_style_set_pad_ver(&g_s.form_row, 6);
    lv_style_set_pad_hor(&g_s.form_row, 0);
    lv_style_set_pad_gap(&g_s.form_row, 10);

    // Dots -----------------------------------------------------------
    make_dot(&g_s.dot_empty,    color::slot_empty());
    make_dot(&g_s.dot_occupied, color::slot_occupied());
    make_dot(&g_s.dot_target,   color::slot_target());
    make_dot(&g_s.dot_picked,   color::slot_picked());
    make_dot(&g_s.dot_error,    color::slot_error());
    make_dot(&g_s.dot_warn,     color::slot_warn());

    // Modal ----------------------------------------------------------
    lv_style_init(&g_s.modal_box);
    lv_style_set_bg_color(&g_s.modal_box, color::surface());
    lv_style_set_bg_opa(&g_s.modal_box, LV_OPA_COVER);
    lv_style_set_radius(&g_s.modal_box, layout::RADIUS);
    lv_style_set_border_color(&g_s.modal_box, color::slot_error());
    lv_style_set_border_width(&g_s.modal_box, 2);
    lv_style_set_pad_all(&g_s.modal_box, 0);

    lv_style_init(&g_s.modal_box_warn);
    lv_style_set_border_color(&g_s.modal_box_warn, color::slot_warn());

    lv_style_init(&g_s.modal_box_info);
    lv_style_set_border_color(&g_s.modal_box_info, color::accent());

    auto modal_head = [](lv_style_t* st, lv_color_t bg) {
        lv_style_init(st);
        lv_style_set_bg_color(st, bg);
        lv_style_set_bg_opa(st, LV_OPA_COVER);
        lv_style_set_text_color(st, color::text_on_accent());
        lv_style_set_text_font(st, &lv_font_montserrat_14);
        lv_style_set_pad_hor(st, 16);
        lv_style_set_pad_ver(st, 10);
        lv_style_set_pad_gap(st, 10);
        lv_style_set_radius(st, 0);
        lv_style_set_border_width(st, 0);
    };
    modal_head(&g_s.modal_header_error, color::slot_error());
    modal_head(&g_s.modal_header_warn,  color::slot_warn());
    modal_head(&g_s.modal_header_info,  color::accent());

    // Progress bar ---------------------------------------------------
    lv_style_init(&g_s.progress_bg);
    lv_style_set_bg_color(&g_s.progress_bg, color::border());
    lv_style_set_bg_opa(&g_s.progress_bg, LV_OPA_COVER);
    lv_style_set_radius(&g_s.progress_bg, 3);
    lv_style_set_border_width(&g_s.progress_bg, 0);

    lv_style_init(&g_s.progress_fill);
    lv_style_set_bg_color(&g_s.progress_fill, color::accent());
    lv_style_set_bg_opa(&g_s.progress_fill, LV_OPA_COVER);
    lv_style_set_radius(&g_s.progress_fill, 3);
    lv_style_set_border_width(&g_s.progress_fill, 0);
}

} // namespace theme
