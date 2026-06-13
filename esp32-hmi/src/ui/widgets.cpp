#include "ui/widgets.h"
#include "ui/theme.h"

#include <stdio.h>

namespace ui {

using namespace theme;

// ---------------------------------------------------------------------
// Card
// ---------------------------------------------------------------------
lv_obj_t* card(lv_obj_t* parent, int pad) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    if (pad < 0 || pad == layout::CARD_PAD)
        lv_obj_add_style(c, const_cast<lv_style_t*>(&theme::s().card), 0);
    else if (pad == 10)
        lv_obj_add_style(c, const_cast<lv_style_t*>(&theme::s().card_pad_tight), 0);
    else {
        lv_obj_add_style(c, const_cast<lv_style_t*>(&theme::s().card), 0);
        lv_obj_set_style_pad_all(c, pad, 0);
    }
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t* card_head(lv_obj_t* parent, const char* uppercase_label,
                    const char* right_meta) {
    lv_obj_t* head = lv_obj_create(parent);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_bottom(head, 8, 0);

    lv_obj_t* l = lv_label_create(head);
    lv_label_set_text(l, uppercase_label);
    lv_obj_set_style_text_color(l, color::text_muted(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);

    if (right_meta && right_meta[0]) {
        lv_obj_t* r = lv_label_create(head);
        lv_label_set_text(r, right_meta);
        lv_obj_set_style_text_color(r, color::text_muted(), 0);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_24, 0);
    }
    return head;
}

// ---------------------------------------------------------------------
// Dot grid
// ---------------------------------------------------------------------
static lv_style_t* dot_style_for(app::SlotState st) {
    switch (st) {
        case app::SlotState::OCCUPIED: return const_cast<lv_style_t*>(&theme::s().dot_occupied);
        case app::SlotState::TARGET:   return const_cast<lv_style_t*>(&theme::s().dot_target);
        case app::SlotState::PICKED:   return const_cast<lv_style_t*>(&theme::s().dot_picked);
        case app::SlotState::ERROR:    return const_cast<lv_style_t*>(&theme::s().dot_error);
        case app::SlotState::WARN:     return const_cast<lv_style_t*>(&theme::s().dot_warn);
        case app::SlotState::EMPTY:
        default:                       return const_cast<lv_style_t*>(&theme::s().dot_empty);
    }
}

struct DotClickCtx {
    int slot_num;
    void (*cb)(int slot_num);
};

static void dot_click_cb(lv_event_t* e) {
    auto* ctx = static_cast<DotClickCtx*>(lv_event_get_user_data(e));
    if (ctx && ctx->cb) ctx->cb(ctx->slot_num);
}

static void dot_ctx_free_cb(lv_event_t* e) {
    auto* ctx = static_cast<DotClickCtx*>(lv_event_get_user_data(e));
    delete ctx;
}

lv_obj_t* dot_grid_card(lv_obj_t* parent, const DotGridOpts& opts) {
    lv_obj_t* c = card(parent, 10);
    lv_obj_set_width(c, LV_PCT(100));

    // Header row
    app::State& st = app::state();
    char summary[64] = "";
    int occ = app::slots_occupied();
    int emp = app::slots_empty();
    int n   = st.n_rack;
    snprintf(summary, sizeof(summary),
             "%d occupied  %d empty  %d total", occ, emp, n);
    card_head(c, opts.header_label, summary);

    // Dynamic layout: one labelled row per populated port; each logical
    // slot is one dot, widened for combined (double/triple-width) slots,
    // wrapping within the port row.
    lv_obj_t* list = lv_obj_create(c);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    if (n == 0) {
        lv_obj_t* none = lv_label_create(list);
        lv_label_set_text(none, "No reel modules detected");
        lv_obj_set_style_text_color(none, color::text_muted(), 0);
        lv_obj_set_style_text_font(none, &lv_font_montserrat_24, 0);
    }

    for (int p = 0; p < app::N_PORTS; ++p) {
        bool any = false;
        for (int i = 0; i < n; ++i) if (st.rack[i].port == p) { any = true; break; }
        if (!any) continue;

        lv_obj_t* row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char cl[6]; snprintf(cl, sizeof(cl), "P%d", p + 1);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, cl);
        lv_obj_set_style_text_color(lbl, color::text_muted(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_width(lbl, 46);   // "P4" at 24pt

        lv_obj_t* dots = lv_obj_create(row);
        lv_obj_remove_style_all(dots);
        lv_obj_set_size(dots, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(dots, 1);
        lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(dots, 4, 0);
        lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < n; ++i) {
            const app::Slot& s = st.rack[i];
            if (s.port != p) continue;
            const bool clickable = opts.clickable && s.state == app::SlotState::TARGET;
            lv_obj_t* dot = clickable ? lv_btn_create(dots) : lv_obj_create(dots);
            lv_obj_remove_style_all(dot);
            lv_obj_add_style(dot, dot_style_for(s.state), 0);
            lv_obj_set_size(dot, 22 * s.width + 4 * (s.width - 1), 22);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            if (clickable && opts.on_dot_click) {
                auto* ctx = new DotClickCtx{ s.slot, opts.on_dot_click };
                lv_obj_add_event_cb(dot, dot_click_cb,    LV_EVENT_CLICKED, ctx);
                lv_obj_add_event_cb(dot, dot_ctx_free_cb, LV_EVENT_DELETE,  ctx);
            }
        }
    }

    // Legend
    if (opts.show_legend) {
        lv_obj_t* legend = lv_obj_create(c);
        lv_obj_remove_style_all(legend);
        lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(legend, 12, 0);
        lv_obj_set_style_pad_top(legend, 8, 0);
        lv_obj_clear_flag(legend, LV_OBJ_FLAG_SCROLLABLE);

        struct L { const char* name; lv_color_t col; };
        const L items[] = {
            {"Empty",    color::slot_empty()},
            {"Occupied", color::slot_occupied()},
            {"Target",   color::slot_target()},
            {"Picked",   color::slot_picked()},
            {"Error",    color::slot_error()},
        };
        for (auto& it : items) {
            lv_obj_t* item = lv_obj_create(legend);
            lv_obj_remove_style_all(item);
            lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START,
                                       LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(item, 4, 0);
            lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* sw = lv_obj_create(item);
            lv_obj_remove_style_all(sw);
            lv_obj_set_size(sw, 9, 9);
            lv_obj_set_style_bg_color(sw, it.col, 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(sw, 2, 0);
            lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* l = lv_label_create(item);
            lv_label_set_text(l, it.name);
            lv_obj_set_style_text_color(l, color::text_muted(), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        }
    }
    return c;
}

// ---------------------------------------------------------------------
// Row
// ---------------------------------------------------------------------
static lv_style_t* stripe_style_for(app::SlotState st) {
    switch (st) {
        case app::SlotState::OCCUPIED: return const_cast<lv_style_t*>(&theme::s().row_stripe_occupied);
        case app::SlotState::TARGET:   return const_cast<lv_style_t*>(&theme::s().row_stripe_target);
        case app::SlotState::PICKED:   return const_cast<lv_style_t*>(&theme::s().row_stripe_picked);
        case app::SlotState::ERROR:    return const_cast<lv_style_t*>(&theme::s().row_stripe_error);
        default:                       return const_cast<lv_style_t*>(&theme::s().row_stripe_empty);
    }
}

lv_obj_t* row_make(lv_obj_t* parent, const RowOpts& opts) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, const_cast<lv_style_t*>(&theme::s().row), 0);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Spacing between rows is provided by the parent scroller's
    // pad_gap (set in row_scroller in common.cpp) since LVGL 8 does
    // not expose a margin property.
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Stripe (left-edge 4x28 colored block)
    lv_obj_t* stripe = lv_obj_create(row);
    lv_obj_remove_style_all(stripe);
    if (opts.use_accent_stripe) {
        lv_obj_add_style(stripe, const_cast<lv_style_t*>(&theme::s().row_stripe_accent), 0);
    } else {
        lv_obj_add_style(stripe, stripe_style_for(opts.stripe_state), 0);
    }
    lv_obj_set_size(stripe, 4, 28);
    lv_obj_clear_flag(stripe, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void row_add_slot_num(lv_obj_t* row, const char* text) {
    lv_obj_t* l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color::text(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_min_width(l, 84, 0);   // "#64" / "BO-0042" at 28pt
}

void row_add_main_two_line(lv_obj_t* row, const char* primary, const char* meta) {
    lv_obj_t* col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(col, LV_SIZE_CONTENT);

    lv_obj_t* l1 = lv_label_create(col);
    lv_label_set_text(l1, primary);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l1, color::text(), 0);
    lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l1, LV_PCT(100));

    if (meta && meta[0]) {
        lv_obj_t* l2 = lv_label_create(col);
        lv_label_set_text(l2, meta);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l2, color::text_muted(), 0);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l2, LV_PCT(100));
    }
}

void row_add_qty_two_line(lv_obj_t* row, const char* num, const char* unit) {
    lv_obj_t* col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(col, 110, LV_SIZE_CONTENT);   // "5000" at 28pt
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* l1 = lv_label_create(col);
    lv_label_set_text(l1, num);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l1, color::text(), 0);
    lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(l1, LV_PCT(100));

    if (unit && unit[0]) {
        lv_obj_t* l2 = lv_label_create(col);
        lv_label_set_text(l2, unit);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l2, color::text_muted(), 0);
        lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(l2, LV_PCT(100));
    }
}

lv_obj_t* row_add_button(lv_obj_t* row, const char* text,
                         lv_event_cb_t cb, void* user,
                         bool muted, bool success, bool disabled) {
    lv_obj_t* b = lv_btn_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, const_cast<lv_style_t*>(&theme::s().row_btn), 0);
    if (muted)    lv_obj_add_style(b, const_cast<lv_style_t*>(&theme::s().row_btn_muted),   0);
    if (success)  lv_obj_add_style(b, const_cast<lv_style_t*>(&theme::s().row_btn_success), 0);
    if (disabled) {
        lv_obj_add_style(b, const_cast<lv_style_t*>(&theme::s().btn_disabled), 0);
        lv_obj_add_state(b, LV_STATE_DISABLED);
    }
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    if (cb && !disabled) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

// ---------------------------------------------------------------------
// Tile (home / configure menu primary buttons)
// ---------------------------------------------------------------------
lv_obj_t* tile(lv_obj_t* parent, const TileOpts& o) {
    lv_obj_t* t = lv_btn_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_add_style(t, const_cast<lv_style_t*>(&theme::s().tile), 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    if (o.on_click) lv_obj_add_event_cb(t, o.on_click, LV_EVENT_CLICKED, o.user);

    lv_obj_t* lbl = lv_label_create(t);
    lv_label_set_text(lbl, o.label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl, color::text(), 0);

    if (o.sublabel && o.sublabel[0]) {
        lv_obj_t* sub = lv_label_create(t);
        lv_label_set_text(sub, o.sublabel);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(sub, color::text_muted(), 0);
        // Wrap inside the tile instead of clipping at the right edge.
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sub, LV_PCT(100));
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    }
    return t;
}

// ---------------------------------------------------------------------
// Menu item (configure 3-col grid)
// ---------------------------------------------------------------------
lv_obj_t* menu_item(lv_obj_t* parent, const char* name, const char* desc,
                    lv_event_cb_t cb, void* user) {
    lv_obj_t* m = lv_btn_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_add_style(m, const_cast<lv_style_t*>(&theme::s().tile), 0);
    lv_obj_set_style_pad_all(m, 14, 0);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(m, 4, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(m, cb, LV_EVENT_CLICKED, user);

    lv_obj_t* n = lv_label_create(m);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(n, color::text(), 0);

    lv_obj_t* d = lv_label_create(m);
    lv_label_set_text(d, desc);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(d, color::text_muted(), 0);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d, LV_PCT(100));
    return m;
}

// ---------------------------------------------------------------------
// Plain button
// ---------------------------------------------------------------------
lv_obj_t* button(lv_obj_t* parent, const char* text, BtnKind kind,
                 lv_event_cb_t cb, void* user) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, const_cast<lv_style_t*>(&theme::s().btn), 0);
    if (kind == BtnKind::Primary) lv_obj_add_style(b, const_cast<lv_style_t*>(&theme::s().btn_primary), 0);
    if (kind == BtnKind::Success) lv_obj_add_style(b, const_cast<lv_style_t*>(&theme::s().btn_success), 0);
    if (kind == BtnKind::Danger)  lv_obj_add_style(b, const_cast<lv_style_t*>(&theme::s().btn_danger),  0);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

// ---------------------------------------------------------------------
// Form helpers
// ---------------------------------------------------------------------
lv_obj_t* form_card(lv_obj_t* parent, const char* heading) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_add_style(c, const_cast<lv_style_t*>(&theme::s().form_card), 0);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(c, 0, 0);
    // Spacing between cards comes from the form_scroller's pad_gap.
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    if (heading && heading[0]) {
        lv_obj_t* h = lv_label_create(c);
        lv_label_set_text(h, heading);
        lv_obj_set_style_text_color(h, color::text_muted(), 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_24, 0);
        lv_obj_set_style_pad_bottom(h, 8, 0);
    }
    return c;
}

lv_obj_t* form_row(lv_obj_t* card_body) {
    lv_obj_t* r = lv_obj_create(card_body);
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, const_cast<lv_style_t*>(&theme::s().form_row), 0);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void form_row_label(lv_obj_t* row, const char* label, const char* desc) {
    lv_obj_t* col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(col, LV_PCT(50), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* l = lv_label_create(col);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, color::text(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);

    if (desc && desc[0]) {
        lv_obj_t* d = lv_label_create(col);
        lv_label_set_text(d, desc);
        lv_obj_set_style_text_color(d, color::text_muted(), 0);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_24, 0);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, LV_PCT(100));
    }
}

lv_obj_t* form_input(lv_obj_t* row, const char* value, int width, bool small) {
    lv_obj_t* box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, const_cast<lv_style_t*>(&theme::s().input), 0);
    lv_obj_set_size(box, small ? 110 : width, 54);   // fits 24pt value text
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* l = lv_label_create(box);
    lv_label_set_text(l, value ? value : "");
    lv_obj_set_style_text_color(l, color::text(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return box;
}

// Toggle (40x22 with thumb on left/right). Uses a custom click handler
// to flip both styles.
struct ToggleCtx {
    lv_obj_t* track;
    lv_obj_t* thumb;
    bool      on;
    bool*     state_out;
};

static void toggle_apply(ToggleCtx* c) {
    if (c->on) {
        lv_obj_add_style(c->track, const_cast<lv_style_t*>(&theme::s().toggle_track_on), 0);
        lv_obj_align(c->thumb, LV_ALIGN_RIGHT_MID, -2, 0);
    } else {
        lv_obj_remove_style(c->track, const_cast<lv_style_t*>(&theme::s().toggle_track_on), 0);
        lv_obj_align(c->thumb, LV_ALIGN_LEFT_MID, 2, 0);
    }
    if (c->state_out) *c->state_out = c->on;
}

static void toggle_click_cb(lv_event_t* e) {
    auto* c = static_cast<ToggleCtx*>(lv_event_get_user_data(e));
    if (!c) return;
    c->on = !c->on;
    toggle_apply(c);
}

static void toggle_ctx_free_cb(lv_event_t* e) {
    delete static_cast<ToggleCtx*>(lv_event_get_user_data(e));
}

lv_obj_t* form_toggle(lv_obj_t* row, bool on, bool* state_out) {
    lv_obj_t* track = lv_btn_create(row);
    lv_obj_remove_style_all(track);
    lv_obj_add_style(track, const_cast<lv_style_t*>(&theme::s().toggle_track), 0);
    lv_obj_set_size(track, 40, 22);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* thumb = lv_obj_create(track);
    lv_obj_remove_style_all(thumb);
    lv_obj_add_style(thumb, const_cast<lv_style_t*>(&theme::s().toggle_thumb), 0);
    lv_obj_set_size(thumb, 18, 18);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);

    auto* c = new ToggleCtx{track, thumb, on, state_out};
    toggle_apply(c);
    lv_obj_add_event_cb(track, toggle_click_cb,    LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(track, toggle_ctx_free_cb, LV_EVENT_DELETE,  c);
    return track;
}

// ---------------------------------------------------------------------
// Progress bar
// ---------------------------------------------------------------------
lv_obj_t* progress_bar(lv_obj_t* parent, int percent) {
    lv_obj_t* outer = lv_obj_create(parent);
    lv_obj_remove_style_all(outer);
    lv_obj_add_style(outer, const_cast<lv_style_t*>(&theme::s().progress_bg), 0);
    lv_obj_set_size(outer, LV_PCT(100), 6);
    lv_obj_clear_flag(outer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* fill = lv_obj_create(outer);
    lv_obj_remove_style_all(fill);
    lv_obj_add_style(fill, const_cast<lv_style_t*>(&theme::s().progress_fill), 0);
    lv_obj_set_height(fill, 6);
    lv_obj_set_user_data(outer, fill);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);

    progress_bar_set(outer, percent);
    return outer;
}

void progress_bar_set(lv_obj_t* bar, int percent) {
    if (!bar) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    auto* fill = static_cast<lv_obj_t*>(lv_obj_get_user_data(bar));
    if (fill) lv_obj_set_width(fill, LV_PCT(percent));
}

// ---------------------------------------------------------------------
// Empty state
// ---------------------------------------------------------------------
lv_obj_t* empty_state(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color::text_muted(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_pad_all(l, 20, 0);
    return l;
}

// ---------------------------------------------------------------------
// Notice banner
// ---------------------------------------------------------------------
lv_obj_t* banner(lv_obj_t* parent, const char* text, bool warn) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(box, layout::RADIUS, 0);
    lv_obj_set_style_bg_color(box, warn ? color::slot_warn() : color::surface(), 0);
    lv_obj_set_style_bg_opa(box, warn ? LV_OPA_20 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, warn ? color::slot_warn() : color::border(), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* l = lv_label_create(box);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, warn ? color::slot_warn() : color::text_muted(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    return box;
}

} // namespace ui
