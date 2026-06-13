// CONFIG-FWUPDATE -- firmware versions (HMI + Core) and update actions.
//
//   - Shows the running HMI image (OTA partition + app version) and the
//     Core's version queried live over RS485 (GET_VERSION).
//   - Shows which update images are present on the SD card.
//   - "Update HMI"  -> self-OTA from /sdcard/hmi.bin, then reboot.
//   - "Update Core" -> push /sdcard/core.bin to the RP2040 over RS485.
//
// The updates take several seconds and must NOT run on the LVGL task, so
// a one-shot worker task does the work and reports progress/results back
// via dispatch_on_lvgl.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/app_state.h"
#include "ui/screen_manager.h"
#include "util/lvgl_async.h"
#include "storage/sdcard.h"
#include "rs485/rs485.h"
#include "fw/fw_update.h"
#include "fw/fw_core_update.h"

#include <Arduino.h>
#include <stdio.h>

namespace ui::screens {

using namespace theme;

// ---- Update flow state (single update at a time) -------------------
static lv_obj_t*     s_modal   = nullptr;   // full-screen progress overlay
static lv_obj_t*     s_bar     = nullptr;
static volatile int  s_pct     = 0;
static volatile int  s_result  = 0;
static bool          s_target_hmi = false;

// ---- Progress (called from the worker task) ------------------------
static void apply_progress(void*) { if (s_bar) progress_bar_set(s_bar, s_pct); }
static void on_progress(size_t d, size_t t) {
    s_pct = t ? (int)(100 * d / t) : 0;
    ui::dispatch_on_lvgl(apply_progress, nullptr);
}

static void close_modal() {
    if (s_modal) { lv_obj_del(s_modal); s_modal = nullptr; s_bar = nullptr; }
}

static void show_progress_modal(const char* title) {
    s_pct = 0;
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_50, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* c = card(s_modal, 16);
    lv_obj_set_width(c, 380);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_center(c);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(c, 12, 0);

    lv_obj_t* t = lv_label_create(c);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, color::text(), 0);

    s_bar = progress_bar(c, 0);

    lv_obj_t* cap = lv_label_create(c);
    lv_label_set_text(cap, "Do not power off the device.");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cap, color::text_muted(), 0);
}

// ---- Result message box --------------------------------------------
static void on_result_btn(lv_event_t* e) {
    lv_msgbox_close((lv_obj_t*)lv_event_get_current_target(e));
}
static void show_result(const char* title, const char* body) {
    static const char* btns[] = { "OK", "" };
    lv_obj_t* mb = lv_msgbox_create(lv_layer_top(), title, body, btns, true);
    lv_obj_add_event_cb(mb, on_result_btn, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_center(mb);
}

// ---- Completion handlers (run on the LVGL task) --------------------
static void on_core_done(void*) {
    close_modal();
    fw::CoreResult r = (fw::CoreResult)s_result;
    if (r == fw::CoreResult::Ok)
        show_result("Core updated", "The RP2040 rebooted into the new image.");
    else
        show_result("Core update failed", fw::core_result_str(r));
    ui::rebuild_current();   // refresh the version readouts
}
static void on_hmi_failed(void*) {
    close_modal();
    show_result("HMI update failed", fw::result_str((fw::Result)s_result));
}

// ---- Worker task ---------------------------------------------------
static void fw_worker(void* arg) {
    const bool hmi = (intptr_t)arg != 0;
    if (hmi) {
        fw::Result r = fw::update_hmi_from_sd("hmi.bin", on_progress);
        if (r == fw::Result::Ok) { delay(300); ESP.restart(); }   // boots new image
        s_result = (int)r;
        ui::dispatch_on_lvgl(on_hmi_failed, nullptr);
    } else {
        fw::CoreResult r = fw::update_core_from_sd("core.bin", on_progress);
        s_result = (int)r;
        ui::dispatch_on_lvgl(on_core_done, nullptr);
    }
    vTaskDelete(nullptr);
}

// ---- Confirm + kickoff ---------------------------------------------
static void on_confirm_btn(lv_event_t* e) {
    lv_obj_t* mb = (lv_obj_t*)lv_event_get_current_target(e);
    uint16_t idx = lv_msgbox_get_active_btn(mb);
    lv_msgbox_close(mb);
    if (idx != 1) return;   // not "Update"
    show_progress_modal(s_target_hmi ? "Updating HMI firmware..."
                                     : "Updating Core over RS485...");
    xTaskCreate(fw_worker, "fw-upd", 8192,
                (void*)(intptr_t)(s_target_hmi ? 1 : 0), 1, nullptr);
}

static void show_confirm(bool hmi) {
    s_target_hmi = hmi;
    static const char* btns[] = { "Cancel", "Update", "" };
    const char* body = hmi
        ? "Flash /sdcard/hmi.bin into the inactive OTA slot and reboot the HMI?"
        : "Push /sdcard/core.bin to the RP2040 over RS485 and reboot it into the new image?";
    lv_obj_t* mb = lv_msgbox_create(lv_layer_top(),
                                    hmi ? "Update HMI" : "Update Core",
                                    body, btns, false);
    lv_obj_add_event_cb(mb, on_confirm_btn, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_center(mb);
}

static size_t sd_size(const char* fname) {
    if (!sdcard::mounted()) return 0;
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", sdcard::mount_point(), fname);
    return sdcard::file_size(path);
}

static void on_update_hmi(lv_event_t*) {
    if (!sd_size("hmi.bin")) { show_result("No image", "/sdcard/hmi.bin not found."); return; }
    show_confirm(true);
}
static void on_update_core(lv_event_t*) {
    if (!sd_size("core.bin")) { show_result("No image", "/sdcard/core.bin not found."); return; }
    show_confirm(false);
}

// ---- Small helpers for value rows ----------------------------------
static lv_obj_t* value_row(lv_obj_t* fc, const char* label, const char* desc,
                           const char* value, lv_color_t col) {
    lv_obj_t* r = form_row(fc);
    form_row_label(r, label, desc);
    lv_obj_t* l = lv_label_create(r);
    lv_label_set_text(l, value);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

// ---- Deferred SD-image version scan --------------------------------
// Reading the embedded tag means scanning 100s of KB off the SD card.
// Doing that during build_config_fwupdate() would block the LVGL thread.
// Instead we show the size immediately + "checking..." and let a worker
// scan the files, then fill the labels via dispatch_on_lvgl. A
// generation counter (bumped each build) + a current-screen check make
// stale completions safe to drop.
static lv_obj_t* s_sd_lbl[2];
static char      s_sd_fname[2][16];
static char      s_sd_proj[2][8];
static unsigned  s_sd_kb[2];
static int       s_sd_n;
static uint32_t  s_sd_gen;

struct ScanJob {
    uint32_t gen;
    int      n;
    char     fname[2][16];
    char     proj[2][8];
    unsigned kb[2];
    char     result[2][48];
};

static void apply_sd_versions(void* arg) {
    ScanJob* j = (ScanJob*)arg;
    if (j->gen == s_sd_gen && ui::current() == ui::Screen::ConfigFwupdate) {
        for (int i = 0; i < j->n && i < s_sd_n; ++i)
            if (s_sd_lbl[i]) lv_label_set_text(s_sd_lbl[i], j->result[i]);
    }
    free(j);
}

static void sd_scan_worker(void* arg) {
    ScanJob* j = (ScanJob*)arg;
    for (int i = 0; i < j->n; ++i) {
        char ver[16];
        if (fw::file_version(j->fname[i], j->proj[i], ver, sizeof(ver)))
            snprintf(j->result[i], sizeof(j->result[i]), "v%s  (%u KB)", ver, j->kb[i]);
        else
            snprintf(j->result[i], sizeof(j->result[i]), "no version tag  (%u KB)", j->kb[i]);
    }
    ui::dispatch_on_lvgl(apply_sd_versions, j);
    vTaskDelete(nullptr);
}

// Show the update image on the SD: size now, version filled in async.
static void sd_row(lv_obj_t* fc, const char* fname, const char* project) {
    size_t sz = sd_size(fname);   // quick: open + seek-to-end, no content read
    char label[40]; snprintf(label, sizeof(label), "On SD (%s)", fname);
    if (!sz) {
        value_row(fc, label, nullptr, "not present", color::text_muted());
        return;
    }
    unsigned kb = (unsigned)((sz + 1023) / 1024);
    char val[48]; snprintf(val, sizeof(val), "checking...  (%u KB)", kb);
    lv_obj_t* l = value_row(fc, label, nullptr, val, color::slot_picked());
    if (s_sd_n < 2) {
        s_sd_lbl[s_sd_n] = l;
        snprintf(s_sd_fname[s_sd_n], sizeof(s_sd_fname[0]), "%s", fname);
        snprintf(s_sd_proj[s_sd_n],  sizeof(s_sd_proj[0]),  "%s", project);
        s_sd_kb[s_sd_n] = kb;
        s_sd_n++;
    }
}

static lv_obj_t* action_row(lv_obj_t* fc) {
    lv_obj_t* r = lv_obj_create(fc);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(r, 8, 0);
    lv_obj_set_style_pad_ver(r, 4, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void build_config_fwupdate(lv_obj_t* body) {
    // New generation -- invalidates any in-flight scan from a prior build.
    s_sd_gen++;
    s_sd_n = 0;

    lv_obj_t* sc = form_scroller(body);

    // ---- HMI firmware -------------------------------------------
    {
        lv_obj_t* fc = form_card(sc, "HMI FIRMWARE (ESP32)");

        char run[40];
        snprintf(run, sizeof(run), "v%s", fw::version_str());
        value_row(fc, "Running", fw::build_str(), run, color::text());
        value_row(fc, "Partition", nullptr, fw::running_partition_label(), color::text_muted());
        sd_row(fc, "hmi.bin", "hmi");

        lv_obj_t* ar = action_row(fc);
        button(ar, "Update HMI", BtnKind::Primary, on_update_hmi);
    }

    // ---- Core firmware ------------------------------------------
    {
        lv_obj_t* fc = form_card(sc, "CORE FIRMWARE (RP2040)");

        // Read the version the POLL task already cached -- no blocking
        // RS485 transaction on the LVGL thread (that would stall/glitch
        // the screen build).
        rs485::CoreVersion cv;
        char ver[48];
        lv_color_t ver_col;
        if (rs485::cached_version(cv)) {
            snprintf(ver, sizeof(ver), "v%u.%u.%u", cv.fw_major, cv.fw_minor, cv.fw_patch);
            ver_col = color::text();
        } else {
            snprintf(ver, sizeof(ver), "querying...");
            ver_col = color::text_muted();
        }
        value_row(fc, "Running", "Queried over RS485", ver, ver_col);
        sd_row(fc, "core.bin", "core");

        lv_obj_t* ar = action_row(fc);
        button(ar, "Update Core", BtnKind::Primary, on_update_core);
    }

    // Kick off the (off-thread) SD version scan for any present images.
    if (s_sd_n > 0) {
        ScanJob* j = (ScanJob*)calloc(1, sizeof(ScanJob));
        if (j) {
            j->gen = s_sd_gen;
            j->n   = s_sd_n;
            for (int i = 0; i < s_sd_n; ++i) {
                memcpy(j->fname[i], s_sd_fname[i], sizeof(j->fname[0]));
                memcpy(j->proj[i],  s_sd_proj[i],  sizeof(j->proj[0]));
                j->kb[i] = s_sd_kb[i];
            }
            xTaskCreate(sd_scan_worker, "sd-ver", 4096, j, 1, nullptr);
        }
    }
}

} // namespace ui::screens
