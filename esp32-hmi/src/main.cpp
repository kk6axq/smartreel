// =====================================================================
//  Reel Rack HMI -- entry point.
//
//  Bring-up order matters:
//    1) Wire (I2C)         -- so the CH422G expander can answer.
//    2) CH422G              -- everything below it (LCD, touch) needs
//                              the expander's reset / backlight pins.
//    3) RGB display + LVGL  -- the framebuffer is the heaviest user
//                              of PSRAM, do this before allocating
//                              any large UI structures.
//    4) GT911 touch         -- registers an LVGL input device.
//    5) SD card             -- best-effort; the UI shows a "NO SD"
//                              warning pill if the card is missing.
//    6) Config store        -- reads /sdcard/config.json + rack.json,
//                              applies to app_state.
//    7) UI                  -- theme, screen manager.
//    8) WiFi manager        -- non-blocking; starts a connection
//                              attempt if credentials are present.
//    9) RS485 master        -- UART1 in half-duplex mode; starts the
//                              background POLL task that pulls async
//                              events off the Core PCB.
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "board/board_pins.h"
#include "board/ch422g.h"
#include "display/display.h"
#include "touch/gt911.h"
#include "storage/sdcard.h"
#include "storage/config_store.h"
#include "storage/state_store.h"
#include "net/wifi_mgr.h"
#include "net/inv_api.h"
#include "rs485/rs485.h"
#include "fw/fw_update.h"
#include "fw/fw_core_update.h"
#include "sensors/qr_scanner.h"
#include "util/lvgl_async.h"
#include "ui/anomaly_modal.h"
#include "ui/status_bar.h"

#include <new>
#include <string.h>

#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"

#include "ui/theme.h"
#include "ui/app_state.h"
#include "ui/status_bar.h"
#include "ui/screen_manager.h"
#include "ui/screens/screens.h"
#include "app/hw_mirror.h"
#include "app/slot_map.h"
#include "app/leds.h"
#include "app/inv_sync.h"

// ===================================================================
// Serial console.
//
// Tiny line-buffered reader on USB serial. Lets us trigger a reboot
// into UART download mode from a running firmware — no more juggling
// BOOT / RESET on the back of the board. The next esptool upload
// succeeds without any button presses.
// ===================================================================

static void enter_download_mode() {
    Serial.println("[console] rebooting into UART download mode...");
    Serial.flush();
    delay(50);
    // RTC_CNTL_FORCE_DOWNLOAD_BOOT (bit 0). On the next reset the ROM
    // bootloader checks this and enters UART download mode as if BOOT
    // were held. Cleared by the bootloader on entry, so this is a
    // one-shot — a subsequent normal reset boots the app again.
    REG_WRITE(RTC_CNTL_OPTION1_REG, 0x1);
    esp_restart();
}

static void handle_console_line(const char* line) {
    if (!*line) return;
    if (!strcmp(line, "dl") || !strcmp(line, "bootloader") ||
        !strcmp(line, "download")) {
        enter_download_mode();
        return;
    }
    if (!strcmp(line, "reboot") || !strcmp(line, "reset")) {
        Serial.println("[console] rebooting...");
        Serial.flush();
        delay(50);
        esp_restart();
        return;
    }
    if (!strcmp(line, "stats")) {
        const rs485::Stats& s = rs485::stats();
        Serial.printf("[rs485] tx=%lu rx=%lu crc_err=%lu timeouts=%lu retries=%lu events=%lu\n",
                      (unsigned long)s.tx_frames, (unsigned long)s.rx_frames,
                      (unsigned long)s.crc_errors, (unsigned long)s.timeouts,
                      (unsigned long)s.retries, (unsigned long)s.events_received);
        return;
    }
    if (!strcmp(line, "listen")) {
        uint8_t sample[32]; size_t sn = 0;
        size_t n = rs485::debug_raw_listen(1000, sample, sizeof(sample), &sn);
        Serial.printf("[rs485] listen 1000ms: %u raw bytes; sample:", (unsigned)n);
        for (size_t i = 0; i < sn; ++i) Serial.printf(" %02X", sample[i]);
        Serial.println();
        return;
    }
    if (!strcmp(line, "ping")) {
        rs485::Status st = rs485::ping();
        Serial.printf("[rs485] ping -> %s\n", rs485::status_str(st));
        return;
    }
    if (!strcmp(line, "fwinfo")) {
        Serial.printf("[fw] HMI v%s  built %s  partition=%s  (git %s)\n",
                      fw::version_str(), fw::build_str(),
                      fw::running_partition_label(), fw::running_app_version());
        return;
    }
    if (!strncmp(line, "fwupdate", 8)) {
        char tmp[96];
        strncpy(tmp, line, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
        strtok(tmp, " ");                       // "fwupdate"
        const char* target = strtok(nullptr, " ");
        const char* file   = strtok(nullptr, " ");
        if (!target || !file) {
            Serial.println("[fw] usage: fwupdate hmi <file> | fwupdate core <file>");
            return;
        }
        if (!strcmp(target, "hmi")) {
            Serial.printf("[fw] updating HMI from /sdcard/%s ...\n", file);
            fw::Result r = fw::update_hmi_from_sd(file, [](size_t d, size_t t) {
                static int last = -1;
                int pct = t ? (int)(100 * d / t) : 0;
                if (pct != last && pct % 10 == 0) { Serial.printf("[fw]  %d%%\n", pct); last = pct; }
            });
            if (r == fw::Result::Ok) {
                Serial.println("[fw] staged ok -- rebooting into new image");
                Serial.flush(); delay(100); esp_restart();
            } else {
                Serial.printf("[fw] FAILED: %s (running app unchanged)\n", fw::result_str(r));
            }
        } else if (!strcmp(target, "core")) {
            Serial.printf("[fw] pushing /sdcard/%s to RP2040 over RS485 ...\n", file);
            fw::CoreResult r = fw::update_core_from_sd(file, [](size_t d, size_t t) {
                static int last = -1;
                int pct = t ? (int)(100 * d / t) : 0;
                if (pct != last && pct % 10 == 0) { Serial.printf("[fw]  %d%%\n", pct); last = pct; }
            });
            if (r == fw::CoreResult::Ok)
                Serial.println("[fw] core update committed -- RP2040 rebooting into new image");
            else
                Serial.printf("[fw] core update FAILED: %s\n", fw::core_result_str(r));
        } else {
            Serial.println("[fw] usage: fwupdate hmi <file> | fwupdate core <file>");
        }
        return;
    }
    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        Serial.println("[console] commands:");
        Serial.println("  dl | bootloader | download   reboot into UART download mode");
        Serial.println("  reboot | reset               normal reboot");
        Serial.println("  stats                        RS485 frame counters");
        Serial.println("  ping                         one-shot RS485 ping to Core");
        Serial.println("  fwinfo                       running partition + app version");
        Serial.println("  fwupdate hmi <file>          self-OTA from /sdcard/<file>");
        Serial.println("  fwupdate core <file>         push <file> to RP2040 over RS485");
        Serial.println("  help | ?                     this list");
        return;
    }
    Serial.printf("[console] unknown: '%s' (try 'help')\n", line);
}

static void serial_console_poll() {
    static char   buf[64];
    static size_t len = 0;
    while (Serial.available()) {
        int ch = Serial.read();
        if (ch < 0) break;
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                buf[len] = 0;
                handle_console_line(buf);
                len = 0;
            }
        } else if (len + 1 < sizeof(buf)) {
            buf[len++] = (char)ch;
        } else {
            // overflow -- drop and reset
            len = 0;
        }
    }
}

static void lvgl_task(void* /*arg*/) {
    constexpr TickType_t period = pdMS_TO_TICKS(5);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        lv_timer_handler();
        vTaskDelayUntil(&last, period);
    }
}

// ===================================================================
// RS485 async event dispatcher.
//
// The RS485 POLL task calls on_rs485_event from its own thread. Per
// our threading model (LVGL is the sole writer of app_state), all
// state mutations have to happen on the LVGL task. We parse the
// event payload here in the POLL task, allocate a tiny effect-
// descriptor struct on the heap, and ui::dispatch_on_lvgl it; the
// LVGL-task callback then mutates app_state, persists, and surfaces
// any UI side effects (anomaly modal, etc.).
// ===================================================================

// Heap-passed payloads from the POLL task -> LVGL task.
struct InputChangeEv {
    uint8_t  port, module;
    uint32_t prev, now;       // 32-bit module word (S + D bits)
};
struct TopologyEv {
    uint8_t  port, module_count;
    bool     inserted;        // count went up vs down
};

static uint32_t be32_at(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

// Apply one slot's transition. Runs on the LVGL task; takes the
// app_state lock for its mutations.
static void apply_slot_change(int slot_num, bool now_present) {
    auto& st = app::state();
    app::Slot* slot = app::slot_by_num(slot_num);
    if (!slot) return;

    Serial.printf("[hmi] slot %d presence settled -> %s (state=%d, part=%s)\n",
                  slot_num, now_present ? "PRESENT" : "absent",
                  (int)slot->state, slot->part.valid ? slot->part.id : "-");

    // Was this change part of an active workflow's expectation?
    bool expected = false;

    // Pick workflow: removing a reel from a target slot completes
    // that line item.
    if (!now_present && st.active_pick_idx >= 0
        && slot->state == app::SlotState::TARGET) {
        auto& job = st.pick_jobs[st.active_pick_idx];
        for (int i = 0; i < job.n_items; ++i) {
            if (job.items[i].slot_num == slot_num && !job.items[i].picked) {
                app::lock();
                job.items[i].picked = true;
                // The reel physically left (-> staging in InvenTree), so
                // the slot is now empty. Progress is tracked by the item's
                // picked flag, not slot state.
                slot->state      = app::SlotState::EMPTY;
                slot->part.valid = false;
                slot->qty        = 0;
                app::unlock();
                state_store::mark_jobs_dirty();
                state_store::mark_slot_dirty(slot_num);
                // The reel is out: darken its slot LED now.
                leds::light_slot(slot_num, 0, 0, 0);
                // Report to InvenTree: whole-reel transfer to the job's
                // destination (user story 4). Partial-pick semantics fall
                // out for free -- each removed reel is reported as it
                // happens, so cancelling later moves nothing extra.
                inv_sync::queue_job_pick(job.id, i, slot_num);
                // Reflect the pick immediately on the Pick screen (row ->
                // Done, progress bar advances) instead of waiting for a
                // manual tap or the next rebuild.
                if (ui::current() == ui::Screen::PickActive) ui::rebuild_current();
                expected = true;
                break;
            }
        }
    }

    // Load workflow: inserting a reel into a target slot commits
    // the load. mock_place_reel does the heavy lifting (clears
    // other lit, assigns part, marks dirty).
    if (now_present && st.load_step == app::LoadStep::Placed
        && slot->state == app::SlotState::TARGET) {
        app::mock_place_reel(slot_num);
        leds::clear_all();              // drop the lit placement targets
        ui::navigate(ui::Screen::Home); // finish the load workflow
        expected = true;
    }

    // Pick-out workflow (armed from the View screen): physically pulling
    // the armed reel confirms its removal from inventory -- no tamper
    // fault. Extinguish its LED and refresh the View if it's up.
    if (!now_present && slot_num == st.pick_out_slot) {
        app::finish_pick_out(slot_num);
        leds::light_slot(slot_num, 0, 0, 0);
        if (ui::current() == ui::Screen::View) ui::rebuild_current();
        expected = true;
    }

    if (expected) return;

    // Unexpected change.
    //  - INSERTION: a reel placed without a scan -> a (non-latching)
    //    "added" warning. No part is assigned yet (not inventoried).
    //  - REMOVAL of an INVENTORIED reel (one with an assigned part):
    //    tampering -> latching "removed" fault.
    //  - REMOVAL of a reel that was only inserted-but-never-inventoried:
    //    just undo the accidental insert. Clear its "added" warning and
    //    reset the slot; raise NO fault.
    if (now_present) {
        app::lock();
        slot->state = app::SlotState::WARN;
        app::unlock();
        state_store::mark_slot_dirty(slot_num);
        ui::anomaly_modal_raise(app::AnomalyKind::Added);
        inv_sync::queue_anomaly("added", slot_num, "reel inserted without scan");
        return;
    }

    if (slot->part.valid) {
        app::lock();
        slot->state = app::SlotState::ERROR;
        app::unlock();
        state_store::mark_slot_dirty(slot_num);
        ui::anomaly_modal_raise(app::AnomalyKind::Removed);
        // Reconcile server-side: the reel is physically gone, so its
        // stock moves to the "pulled" bin rather than silently rotting
        // in the slot location (contract: POST /rack/slots/n/clear).
        char detail[64];
        snprintf(detail, sizeof(detail), "unexpected removal of %s", slot->part.id);
        inv_sync::queue_anomaly("removed", slot_num, detail);
        inv_sync::queue_clear(slot_num, "anomaly:removed");
    } else {
        app::lock();
        slot->state = app::SlotState::EMPTY;
        app::unlock();
        state_store::mark_slot_dirty(slot_num);
        // Clear the transient "added" warning raised by the insert, if up.
        if (app::state().anomaly.kind == app::AnomalyKind::Added)
            ui::anomaly_modal_resolve();
    }
}

// Aggregate reel-presence of a logical slot: present if ANY physical
// reel-slot in its (possibly combined) run reads present.
static bool logical_present(const app::Slot& ls) {
    for (int w = 0; w < ls.width; ++w) {
        int pix = ls.module * 16 + ls.mslot + w;
        if (hw_mirror::slot_present(ls.port, pix / 16, pix % 16)) return true;
    }
    return false;
}

// ---- Reel-presence debounce ------------------------------------------
//
// A reel seating or being pulled chatters the presence switch for tens to
// hundreds of ms. Acting on the first edge causes premature/incorrect
// place, pick, and anomaly actions (and a bounce back can flip-flop the
// server). So we hold each logical slot's presence change for
// PRESENCE_DEBOUNCE_MS of stability before committing it to
// apply_slot_change(). A change that reverts within the window is dropped.
//
// All of this lives on the LVGL task (apply_input_change is dispatched
// there, the tick is an lv_timer), so no locking is needed.
static constexpr uint32_t PRESENCE_DEBOUNCE_MS = 600;

struct SlotDebounce {
    bool     known        = false;  // last_present is valid
    bool     last_present = false;  // presence already committed for this slot
    bool     pending      = false;  // a candidate change is settling
    bool     pending_val  = false;  // candidate presence
    uint32_t pending_ms   = 0;      // millis() when pending_val last (re)set
};
static SlotDebounce g_deb[app::MAX_LOGICAL_SLOTS + 1];

// Re-baseline every slot's committed presence to current physical truth.
// Called after boot sync and after any rack rebuild (logical numbering
// can change), so the next real edge debounces from the right starting
// point instead of firing a spurious insert/remove.
static void presence_debounce_reset() {
    for (auto& d : g_deb) { d = SlotDebounce{}; }
    app::State& st = app::state();
    for (int i = 0; i < st.n_rack; ++i) {
        int num = st.rack[i].slot;
        if (num < 0 || num > app::MAX_LOGICAL_SLOTS) continue;
        g_deb[num].known        = true;
        g_deb[num].last_present = logical_present(st.rack[i]);
    }
}

// Record a (possibly bouncing) presence reading for a logical slot.
static void note_presence(int slot_num, bool present) {
    if (slot_num < 0 || slot_num > app::MAX_LOGICAL_SLOTS) return;
    SlotDebounce& d = g_deb[slot_num];
    if (!d.known) { d.known = true; d.last_present = present; return; }
    if (present == d.last_present) { d.pending = false; return; }  // reverted/no-op
    if (!d.pending || d.pending_val != present) {
        d.pending     = true;
        d.pending_val = present;
        d.pending_ms  = millis();   // (re)start the settle clock
    }
}

// 100 ms lv_timer: commit any presence change that has held steady for
// PRESENCE_DEBOUNCE_MS, re-reading live presence at commit time.
static void presence_debounce_tick(lv_timer_t*) {
    const uint32_t now = millis();
    for (int num = 0; num <= app::MAX_LOGICAL_SLOTS; ++num) {
        SlotDebounce& d = g_deb[num];
        if (!d.pending) continue;
        if ((int32_t)(now - d.pending_ms) < (int32_t)PRESENCE_DEBOUNCE_MS) continue;
        const app::Slot* s = app::slot_by_num(num);
        const bool live = s ? logical_present(*s) : d.pending_val;
        d.pending = false;
        if (live != d.last_present) {
            d.last_present = live;
            apply_slot_change(num, live);
        }
    }
}

// A divider bit changed on (port, module). Post-commission this is a
// fault (raise the divider anomaly); pre-commission it re-shapes the
// logical rack (rebuild + refresh the UI live).
static void handle_divider_change(uint8_t port, uint8_t module,
                                  uint32_t prev, uint32_t now) {
    if (config_store::cfg().rack.committed) {
        const auto& div = config_store::cfg().rack.dividers;
        for (int s = 0; s < 16; ++s) {
            if (s == 0 && module == 0) continue;                 // port edge
            const bool changed = ((prev ^ now) >> (2 * s)) & 1u;
            if (!changed) continue;
            const bool expect_present = !div.is_pulled(port, module, (uint8_t)s);
            const bool live_present   = app::divider_present_bit(now, s);
            // DividerMissing: a committed divider was pulled out.
            // DividerExtra:   an unexpected divider was inserted.
            // validate_topology() reports both; flag either as the same
            // divider-mismatch anomaly so the operator restores the layout.
            if (expect_present != live_present) {
                ui::anomaly_modal_raise(app::AnomalyKind::Divider);
                return;
            }
        }
    } else {
        app::rebuild_rack();
        presence_debounce_reset();   // logical numbering may have changed
        ui::mark_all_dirty();
    }
}

static void apply_input_change(void* user) {
    auto* ev = static_cast<InputChangeEv*>(user);
    if (!ev) return;
    hw_mirror::set_inputs(ev->port, ev->module, ev->now);

    // Self Test "Buttons" card: while that screen is up, surface raw bit
    // changes and SKIP the workflow/anomaly logic below (which raises the
    // anomaly modal -- currently crashing the HMI). This lets a reel
    // button or divider be pressed and its decoded slot read cleanly.
    if (ui::current() == ui::Screen::ConfigSelftest) {
        ui::screens::selftest_on_input(ev->port, ev->module, ev->prev, ev->now);
        delete ev;
        return;
    }

    const uint32_t changed = ev->prev ^ ev->now;
    bool divider_changed = false;
    for (int s = 0; s < 16; ++s) {
        if ((changed >> (2 * s + 1)) & 1u) {                    // reel-presence (S) bit
            const app::Slot* ls = app::slot_at_physical(ev->port, ev->module, s);
            // Debounced: note the reading now, commit after it settles.
            if (ls) note_presence(ls->slot, logical_present(*ls));
        }
        if ((changed >> (2 * s)) & 1u) divider_changed = true;  // divider (D) bit
    }
    if (divider_changed) handle_divider_change(ev->port, ev->module, ev->prev, ev->now);
    delete ev;
}

// A module was inserted/removed on a port (topology change).
static void apply_topology(void* user) {
    auto* ev = static_cast<TopologyEv*>(user);
    if (!ev) return;
    hw_mirror::set_module_count(ev->port, ev->module_count);

    // Pull the (now-present) modules' input words so presence/dividers
    // are known. Blocking RS485 read -- fine for a rare topology event.
    uint32_t words[hw_mirror::MAX_MODULES] = { 0 };
    int nmods = 0;
    if (rs485::read_inputs(ev->port, words, hw_mirror::MAX_MODULES, &nmods) == rs485::Status::Ok)
        for (int m = 0; m < nmods && m < hw_mirror::MAX_MODULES; ++m)
            hw_mirror::set_inputs(ev->port, m, words[m]);

    const auto& rc = config_store::cfg().rack;
    if (rc.committed && ev->module_count != rc.module_count[ev->port]) {
        // Topology drifted from the commissioned layout -- warn, keep numbering.
        ui::anomaly_modal_raise(ev->inserted ? app::AnomalyKind::Added
                                             : app::AnomalyKind::Removed);
    } else {
        // Pre-commission: follow the live topology.
        app::rebuild_rack();
        presence_debounce_reset();   // logical numbering may have changed
        ui::mark_all_dirty();
    }
    delete ev;
}

static void on_rs485_event(const rs485::Event& e, void* /*user*/) {
    switch (e.type) {
        case rs485::EVT_INPUT_CHANGE: {
            // 14 B: port(1) module(1) prev32(4 BE) new32(4 BE) ts32(4 BE).
            if (e.payload_len < 14) {
                log_w("rs485 INPUT_CHANGE: short payload_len=%u", e.payload_len);
                break;
            }
            auto* ev = new (std::nothrow) InputChangeEv{
                e.payload[0], e.payload[1],
                be32_at(&e.payload[2]), be32_at(&e.payload[6]) };
            if (ev) ui::dispatch_on_lvgl(apply_input_change, ev);
            break;
        }

        case rs485::EVT_REEL_INSERTED: {
            // 4 B: port(1) module_count(1) sense_mv(2 BE).
            if (e.payload_len < 2) break;
            auto* ev = new (std::nothrow) TopologyEv{ e.payload[0], e.payload[1], true };
            if (ev) ui::dispatch_on_lvgl(apply_topology, ev);
            break;
        }

        case rs485::EVT_REEL_REMOVED: {
            // 2 B: port(1) module_count(1).
            if (e.payload_len < 2) break;
            auto* ev = new (std::nothrow) TopologyEv{ e.payload[0], e.payload[1], false };
            if (ev) ui::dispatch_on_lvgl(apply_topology, ev);
            break;
        }

        case rs485::EVT_SENSE_THRESHOLD:
            log_i("rs485 SENSE_THRESHOLD evt (%u bytes)", e.payload_len);
            break;

        case rs485::EVT_LOG:
            if (e.payload_len >= 2) {
                char buf[160];
                size_t n = e.payload_len - 1;
                if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                memcpy(buf, &e.payload[1], n);
                buf[n] = 0;
                log_i("[core L%u] %s", e.payload[0], buf);
            }
            break;

        default:
            log_w("rs485 unknown event 0x%02X (%u bytes)", e.type, e.payload_len);
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[boot] reel-rack HMI starting");
    Serial.printf("[boot] build " __DATE__ " " __TIME__ ", partition=%s\n",
                  fw::running_partition_label());
    Serial.println("[boot] serial console: type 'help' for commands ('dl' = enter UART download mode)");

    // 1) I2C bus
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);

    // 2) Expander
    if (!ch422g::init()) {
        Serial.println("[boot] CH422G init FAILED");
    }
    delay(20);

    // 3) LVGL core + display
    lv_init();
    if (!display::init()) {
        Serial.println("[boot] display init FAILED -- halting");
        for (;;) { delay(1000); }
    }

    // 4) Touch
    if (!touch::init()) {
        Serial.println("[boot] touch init failed (display still works)");
    }

    // 4b) QR scanner (optional, on the same I2C bus as touch + CH422G)
    if (!qr_scanner::init()) {
        Serial.println("[boot] QR scanner not present at 0x0C (re-probe from Self Test)");
    }

    // 5) SD card (optional)
    bool sd_ok = sdcard::init();
    if (!sd_ok) {
        Serial.println("[boot] SD missing -- running on mock data");
    }

    // 6) Config store: read JSON from SD if available, otherwise use
    //    defaults. apply_to_app_state only overlays non-empty values
    //    so the UI's mock data stays intact when no SD card.
    config_store::init();
    (void)app::state();           // seed mock data
    config_store::apply_to_app_state();

    // 6b) Runtime-state store load is deferred until after the reel
    //     topology is known (step 9b), since rack contents now key to
    //     logical slot numbers that the live/committed topology defines.

    // 6c) Runtime-state writer task. (Loads are resolved live against
    //     InvenTree via inv_api now — no SD parts catalog.)
    if (!state_store::start_writer()) {
        Serial.println("[boot] state_store writer task failed to start");
    }

    // 7) UI
    theme::init();
    ui::init();
    ui::status_bar_set_sd_missing(!sd_ok);
    Serial.println("[boot] UI ready");

    // 8) WiFi (non-blocking; events drive app_state.online + status bar)
    wifi_mgr::init();

    // 8b) InvenTree plugin client (no network traffic; just primes
    //     the chip-id used for op_ids and zeroes the "last health"
    //     cache). Actual HTTP happens lazily.
    inv_api::init();

    // 8c) Background InvenTree sync: health poll, rack registration +
    //     reconciliation, pick-job fetches, and the mutation op queue.
    inv_sync::start();

    // 9) RS485 master + POLL task
    {
        rs485::set_event_handler(on_rs485_event, nullptr);
        rs485::Status rs = rs485::init();
        if (rs != rs485::Status::Ok) {
            Serial.printf("[boot] rs485 init failed: %s\n", rs485::status_str(rs));
        } else {
            Serial.println("[boot] rs485 ready (polling Core)");
        }
    }

    // 9b) Reel topology: read the live module counts + input words from
    //     the Core, build the dynamic rack, then overlay persisted slot
    //     contents (keyed by logical number). If the Core is absent the
    //     rack starts empty and fills in as modules appear.
    if (hw_mirror::sync_from_core())
        Serial.println("[boot] reel topology synced from Core");
    else
        Serial.println("[boot] no reel topology yet (Core absent?)");

    // Boot LED self-test: chase one pixel down each connected reel
    // module, then all off (no-ops if no Core/modules present).
    leds::boot_chase();

    app::rebuild_rack();
    bool state_loaded = state_store::load();
    app::set_boot_loaded_from_sd(state_loaded);
    ui::mark_all_dirty();    // refresh screens built before topology was known

    // Baseline the presence debounce to current physical truth, then run
    // its 100 ms settle tick on the LVGL task.
    presence_debounce_reset();
    lv_timer_create(presence_debounce_tick, 100, nullptr);

    // LVGL runs on its own task pinned to APP_CPU so the RGB DMA
    // refresh on PRO_CPU isn't disrupted.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8 * 1024, nullptr,
                            2, nullptr, APP_CPU_NUM);
}

void loop() {
    // Reserved for application-level work (Inventree poll, RS485 IO,
    // anomaly detection). LVGL runs on its own task, see setup().
    serial_console_poll();
    delay(20);
}
