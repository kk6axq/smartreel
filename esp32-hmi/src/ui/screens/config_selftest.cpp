// CONFIG-SELFTEST -- 3-col grid of test cards.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "ui/anomaly_modal.h"
#include "ui/screen_manager.h"
#include "storage/sdcard.h"

#include <stdio.h>

namespace ui::screens {

using namespace theme;

static void on_anom_removed(lv_event_t*) { ui::anomaly_modal_raise(app::AnomalyKind::Removed); }
static void on_anom_added(lv_event_t*)   { ui::anomaly_modal_raise(app::AnomalyKind::Added); }
static void on_anom_divider(lv_event_t*) { ui::anomaly_modal_raise(app::AnomalyKind::Divider); }

// ---- SD format confirm + result -----------------------------------
// Two-step UX: tap Format -> confirm modal -> sdcard::format() ->
// result modal. Both modals are LVGL message-box widgets parented to
// lv_layer_top so they overlay the screen.
static void on_confirm_format(lv_event_t* e);
static void on_cancel_format(lv_event_t* e);

static void show_format_result(bool ok) {
    static const char* btns[] = { "OK", "" };
    lv_obj_t* mb = lv_msgbox_create(lv_layer_top(),
        ok ? "Format complete" : "Format failed",
        ok ? "The SD card has been wiped and re-formatted as FAT32."
           : "The SD card could not be formatted. Check the serial log.",
        btns, true);
    lv_obj_center(mb);
}

static void on_confirm_format(lv_event_t* e) {
    lv_obj_t* mb = (lv_obj_t*)lv_event_get_current_target(e);
    lv_msgbox_close(mb);
    bool ok = sdcard::format();
    show_format_result(ok);
    // The screen has stale mounted-state references (capacity etc);
    // mark dirty so the next visit re-reads.
    ui::rebuild_current();
}

static void on_cancel_format(lv_event_t* e) {
    lv_obj_t* mb = (lv_obj_t*)lv_event_get_current_target(e);
    lv_msgbox_close(mb);
}

static void on_msgbox_btn(lv_event_t* e) {
    lv_obj_t* mb = (lv_obj_t*)lv_event_get_current_target(e);
    uint16_t idx = lv_msgbox_get_active_btn(mb);
    if (idx == 0)      on_cancel_format(e);
    else if (idx == 1) on_confirm_format(e);
}

static void on_format_clicked(lv_event_t*) {
    static const char* btns[] = { "Cancel", "Format", "" };
    char body[160];
    if (sdcard::mounted()) {
        snprintf(body, sizeof(body),
            "Wipe and re-format the SD card as FAT32?\n\n"
            "All data will be erased. Capacity: %llu MB.",
            sdcard::capacity_bytes() / (1024ULL * 1024ULL));
    } else {
        snprintf(body, sizeof(body),
            "No SD card detected. Insert one, then tap Format to "
            "force a wipe and FAT32 reformat.");
    }
    lv_obj_t* mb = lv_msgbox_create(lv_layer_top(), "Format SD card",
                                     body, btns, false);
    lv_obj_add_event_cb(mb, on_msgbox_btn, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_center(mb);
}

// Compact test-card: title + 1-line description (truncated) + a
// button-row + an inline coloured status. Dense enough to fit a
// 3-row grid in a 440px body without scrolling.
static lv_obj_t* test_card(lv_obj_t* parent, const char* title, const char* desc) {
    lv_obj_t* c = card(parent, 8);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(c, 4, 0);

    lv_obj_t* h = lv_label_create(c);
    lv_label_set_text(h, title);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, color::text(), 0);

    lv_obj_t* d = lv_label_create(c);
    lv_label_set_text(d, desc);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(d, color::text_muted(), 0);
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);   // 1 line, ellipsis
    lv_obj_set_width(d, LV_PCT(100));
    return c;
}

static void status_line(lv_obj_t* card_obj, const char* text, lv_color_t col) {
    lv_obj_t* l = lv_label_create(card_obj);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
}

static lv_obj_t* btn_row(lv_obj_t* parent) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(r, 6, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void build_config_selftest(lv_obj_t* body) {
    lv_obj_t* sc = lv_obj_create(body);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(sc, 8, 0);
    lv_obj_set_layout(sc, LV_LAYOUT_GRID);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
    static lv_coord_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                  LV_GRID_TEMPLATE_LAST };
    static lv_coord_t rows[] = { LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT,
                                  LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(sc, cols, rows);
    lv_obj_set_style_pad_column(sc, 8, 0);
    lv_obj_set_style_pad_row(sc, 8, 0);

    auto place = [sc](lv_obj_t* o, int col, int row) {
        lv_obj_set_grid_cell(o, LV_GRID_ALIGN_STRETCH, col, 1,
                                 LV_GRID_ALIGN_STRETCH, row, 1);
    };

    {
        lv_obj_t* c = test_card(sc, "All LEDs", "Cycle every slot LED");
        lv_obj_t* br = btn_row(c);
        button(br, "Run", BtnKind::Primary);
        button(br, "Off", BtnKind::Default);
        status_line(c, "Passed  18s ago", color::slot_picked());
        place(c, 0, 0);
    }
    {
        lv_obj_t* c = test_card(sc, "Single slot", "Light one LED");
        lv_obj_t* br = btn_row(c);
        form_input(br, "12", 0, true);
        button(br, "Light", BtnKind::Default);
        status_line(c, "Idle", color::text_muted());
        place(c, 1, 0);
    }
    {
        lv_obj_t* c = test_card(sc, "Buttons", "Press any reel button");
        button(c, "Watch", BtnKind::Default);
        status_line(c, "Last: slot 7 reel", color::text_muted());
        place(c, 2, 0);
    }
    {
        lv_obj_t* c = test_card(sc, "RS485 chain", "Ping chained MCUs");
        button(c, "Ping", BtnKind::Primary);
        status_line(c, "4 / 4 chains OK", color::slot_picked());
        place(c, 0, 1);
    }
    {
        lv_obj_t* c = test_card(sc, "QR scanner", "Echo next code");
        button(c, "Echo", BtnKind::Default);
        status_line(c, "Last: R-10K-0805", color::text_muted());
        place(c, 1, 1);
    }
    {
        lv_obj_t* c = test_card(sc, "Anomaly (mock)", "Trigger alert modal");
        lv_obj_t* br = btn_row(c);
        button(br, "Removed", BtnKind::Danger,  on_anom_removed);
        button(br, "Added",   BtnKind::Default, on_anom_added);
        button(br, "Divider", BtnKind::Default, on_anom_divider);
        status_line(c, "Manual triggers", color::text_muted());
        place(c, 2, 1);
    }
    // ---- Maintenance: format SD card ------------------------------
    {
        lv_obj_t* c = test_card(sc, "Format SD card", "Wipe + re-format as FAT32");
        button(c, "Format SD", BtnKind::Danger, on_format_clicked);
        const char* st;
        lv_color_t st_col;
        if (sdcard::mounted()) {
            static char buf[40];
            snprintf(buf, sizeof(buf), "Mounted (%llu MB)",
                     sdcard::capacity_bytes() / (1024ULL * 1024ULL));
            st = buf; st_col = color::slot_picked();
        } else {
            st = "No card detected"; st_col = color::slot_warn();
        }
        status_line(c, st, st_col);
        place(c, 0, 2);
    }
}

} // namespace ui::screens
