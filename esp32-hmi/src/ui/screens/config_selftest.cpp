// CONFIG-SELFTEST -- 3-col grid of test cards.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "ui/anomaly_modal.h"
#include "ui/screen_manager.h"
#include "ui/text_entry_modal.h"
#include "storage/sdcard.h"
#include "app/leds.h"
#include "rs485/rs485.h"

#include <esp32-hal-log.h>
#include <stdio.h>
#include <stdlib.h>

namespace ui::screens {

using namespace theme;

// ---- Live button/divider watch ------------------------------------
// main.cpp routes raw module-word changes here (and pauses the anomaly
// modal) while this screen is current. We decode the first changed bit
// and show its physical position plus the logical slot the firmware
// maps it to -- the bench tool for chasing the divider/slot bit-order
// reversal. Each module word is  D0 S0 D1 S1 ... D15 S15  (D = even
// bit 2k, S = odd bit 2k+1); each 8-channel 74HC165 holds 4 slots.
static lv_obj_t* s_btn_status = nullptr;

void selftest_on_input(uint8_t port, uint8_t module, uint32_t prev, uint32_t now) {
    if (!s_btn_status) return;
    const uint32_t changed = prev ^ now;
    if (!changed) return;

    // Report one bit; prefer a rising edge (a press) if several changed.
    int bit = -1;
    for (int b = 0; b < 32; ++b) {
        if (!((changed >> b) & 1u)) continue;
        bit = b;
        if ((now >> b) & 1u) break;            // press wins over release
    }
    if (bit < 0) return;

    const int  slot    = bit / 2;              // slot-in-module 0..15
    const bool is_div  = (bit & 1) == 0;       // even bit = divider, odd = reel
    const bool pressed = (now >> bit) & 1u;
    const int  reg     = slot / 4;             // which 8-ch 74HC165 (0..3)
    const int  lane    = slot % 4;             // lane within that register

    int logical = 0;
    const app::Slot* ls = app::slot_at_physical(port, module, slot);
    if (ls) logical = ls->slot;

    char buf[224];
    snprintf(buf, sizeof(buf),
        "%s %s\n"
        "port %u  mod %u  slot %d\n"
        "PISO %d  lane %d  (rev %d)\n"
        "bit %d  -> logical %d",
        is_div ? "DIVIDER" : "REEL", pressed ? "DOWN" : "up",
        port, module, slot, reg, lane, 3 - lane, bit, logical);
    lv_label_set_text(s_btn_status, buf);

    // Mirror to serial so presses are captured even without eyes on the LCD.
    log_i("selftest: %s %s port=%u mod=%u slot=%d PISO=%d lane=%d bit=%d -> logical=%d",
          is_div ? "DIVIDER" : "REEL", pressed ? "down" : "up",
          port, module, slot, reg, lane, bit, logical);
}

static void on_buttons_deleted(lv_event_t*) { s_btn_status = nullptr; }

static void on_anom_removed(lv_event_t*) { ui::anomaly_modal_raise(app::AnomalyKind::Removed); }
static void on_anom_added(lv_event_t*)   { ui::anomaly_modal_raise(app::AnomalyKind::Added); }
static void on_anom_divider(lv_event_t*) { ui::anomaly_modal_raise(app::AnomalyKind::Divider); }
static void on_qr_scanner(lv_event_t*)   { ui::navigate(ui::Screen::QrScanner); }

// LED self-tests. Fire-and-forget through the RS485 master. If no
// Core PCB is attached the calls just time out; the UI doesn't block.
static void on_leds_all_white(lv_event_t*) { leds::fill_all(0xFF, 0xFF, 0xFF); }
static void on_leds_all_off(lv_event_t*)   { leds::clear_all(); }
// "Single slot" lights the chosen slot in theme blue. The slot number
// is editable: tap the field to open the numeric entry modal.
static int s_single_slot = 12;

static void on_leds_single(lv_event_t*)     { leds::light_slot(s_single_slot, 0x25, 0x63, 0xEB); }
static void on_leds_single_off(lv_event_t*) { leds::light_slot(s_single_slot, 0, 0, 0); }

static void on_single_slot_saved(const char* v) {
    if (v && v[0]) { int n = atoi(v); if (n >= 1) s_single_slot = n; }
    ui::rebuild_current();
}
static void on_single_slot_tap(lv_event_t*) {
    char buf[8]; snprintf(buf, sizeof(buf), "%d", s_single_slot);
    ui::TextEntryOpts o = {};
    o.title       = "Slot to light";
    o.initial     = buf;
    o.placeholder = "12";
    o.max_len     = 4;
    o.on_save     = on_single_slot_saved;
    ui::text_entry_modal_open(o);
}

// RS485 chain ping: round-trip a PING to the Core. We just log the
// result for now -- a full diag screen with per-chain stats is
// future work.
static void on_rs485_ping(lv_event_t*) {
    rs485::Status s = rs485::ping();
    log_i("rs485 ping: %s", rs485::status_str(s));
}

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
    lv_obj_set_style_text_font(h, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(h, color::text(), 0);

    lv_obj_t* d = lv_label_create(c);
    lv_label_set_text(d, desc);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(d, color::text_muted(), 0);
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);   // 1 line, ellipsis
    lv_obj_set_width(d, LV_PCT(100));
    return c;
}

static void status_line(lv_obj_t* card_obj, const char* text, lv_color_t col) {
    lv_obj_t* l = lv_label_create(card_obj);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
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
        lv_obj_t* c = test_card(sc, "All LEDs", "Solid white on every slot");
        lv_obj_t* br = btn_row(c);
        button(br, "On",  BtnKind::Primary, on_leds_all_white);
        button(br, "Off", BtnKind::Default, on_leds_all_off);
        status_line(c, "RS485 fire-and-forget", color::text_muted());
        place(c, 0, 0);
    }
    {
        lv_obj_t* c = test_card(sc, "Single slot", "Light one slot");
        lv_obj_t* br = btn_row(c);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", s_single_slot);
        lv_obj_t* in = form_input(br, buf, 0, true);
        lv_obj_add_flag(in, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(in, on_single_slot_tap, LV_EVENT_CLICKED, nullptr);
        button(br, "Light", BtnKind::Primary, on_leds_single);
        button(br, "Off",   BtnKind::Default, on_leds_single_off);
        status_line(c, "Tap the number to change", color::text_muted());
        place(c, 1, 0);
    }
    {
        lv_obj_t* c = test_card(sc, "Buttons", "Press a reel / pull a divider");
        s_btn_status = lv_label_create(c);
        lv_label_set_text(s_btn_status, "Waiting for input...\n(modals paused here)");
        lv_obj_set_style_text_font(s_btn_status, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_btn_status, color::text(), 0);
        lv_obj_set_width(s_btn_status, LV_PCT(100));
        lv_obj_add_event_cb(c, on_buttons_deleted, LV_EVENT_DELETE, nullptr);
        place(c, 2, 0);
    }
    {
        lv_obj_t* c = test_card(sc, "RS485 chain", "Ping the Core PCB");
        button(c, "Ping", BtnKind::Primary, on_rs485_ping);
        status_line(c, "Result on serial log", color::text_muted());
        place(c, 0, 1);
    }
    {
        // The Tiny Code Reader is on I2C 0x0C. The dedicated QR
        // Scanner screen polls it at 5 Hz and shows the live log.
        lv_obj_t* c = test_card(sc, "QR scanner", "Watch live decodes");
        button(c, "Open", BtnKind::Primary, on_qr_scanner);
        status_line(c, "I2C 0x0C", color::text_muted());
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
