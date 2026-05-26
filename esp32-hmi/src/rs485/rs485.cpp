// =====================================================================
//  RS485 master implementation.
//
//  Uses the ESP-IDF UART driver in UART_MODE_RS485_HALF_DUPLEX so the
//  driver handles SP3485 DE/RE toggling automatically: DE goes high
//  before the first byte is shifted out and drops after the last byte
//  clears the TX FIFO. This eliminates a class of race conditions you
//  get with manual GPIO + a polled "TX done" check.
//
//  Architecture
//    - A single mutex guards transact() so request/response sequences
//      don't interleave.
//    - The receive side runs entirely synchronous inside transact():
//      we read bytes until a CRC-valid frame is decoded or the
//      deadline expires. The streaming decoder from rs485_frame.cpp
//      handles resync after garbage.
//    - A separate FreeRTOS task issues MSG_POLL every POLL_PERIOD_MS,
//      parses any events from the response, dispatches them, and
//      sends MSG_ACK_EVENTS so the Core can drop them from its ring.
// =====================================================================

#include "rs485/rs485.h"
#include "rs485/rs485_frame.h"
#include "board/board_pins.h"

#include <Arduino.h>
#include <esp32-hal-log.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <string.h>

namespace rs485 {

// ===================================================================
// Module state
// ===================================================================
static constexpr uart_port_t        UART_NUM    = UART_NUM_1;
static constexpr int                UART_RX_BUF = 1024;
static constexpr int                UART_TX_BUF = 1024;

static bool                  s_inited        = false;
static SemaphoreHandle_t     s_bus_mutex     = nullptr;
static uint8_t               s_next_seq      = 1;
static Stats                 s_stats         = {};

static EventHandler          s_event_cb      = nullptr;
static void*                 s_event_user    = nullptr;

static TaskHandle_t          s_poll_task     = nullptr;
static volatile bool         s_poll_paused   = false;

// ===================================================================
// Receive-side scratch state used by transact(). One pending
// transaction at a time; protected by s_bus_mutex.
// ===================================================================
struct RxExpect {
    bool     have_frame;
    uint8_t  addr, seq, type;
    uint8_t  payload[MAX_PAYLOAD];
    size_t   payload_len;
    uint8_t  match_seq;
};
static RxExpect s_rx;

static void on_decoded_frame(uint8_t addr, uint8_t seq, uint8_t type,
                             const uint8_t* payload, size_t payload_len,
                             void* /*user*/) {
    // Only consume frames addressed to the master with the SEQ we are
    // waiting for. Anything else is dropped (could be a stale
    // retransmit from a previous attempt).
    if (addr != ADDR_MASTER) return;
    if (seq  != s_rx.match_seq) return;
    s_rx.have_frame  = true;
    s_rx.addr        = addr;
    s_rx.seq         = seq;
    s_rx.type        = type;
    s_rx.payload_len = payload_len;
    if (payload_len) memcpy(s_rx.payload, payload, payload_len);
}

static Decoder* s_decoder = nullptr;

// ===================================================================
// Status string
// ===================================================================
const char* status_str(Status s) {
    switch (s) {
        case Status::Ok:             return "ok";
        case Status::Timeout:        return "timeout";
        case Status::CrcError:       return "crc-error";
        case Status::BadResponse:    return "bad-response";
        case Status::BufferTooSmall: return "buffer-too-small";
        case Status::NotReady:       return "not-ready";
        case Status::Internal:       return "internal";
    }
    return "?";
}

// ===================================================================
// Low-level UART
// ===================================================================
static bool uart_setup() {
    uart_config_t cfg = {};
    cfg.baud_rate = RS485_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity    = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err;
    err = uart_driver_install(UART_NUM, UART_RX_BUF, UART_TX_BUF,
                              0, nullptr, 0);
    if (err != ESP_OK) { log_e("uart_driver_install: 0x%x", err); return false; }
    err = uart_param_config(UART_NUM, &cfg);
    if (err != ESP_OK) { log_e("uart_param_config: 0x%x",   err); return false; }
    // GPIO43/44 are the chip's default UART0 console pins. GPIO43
    // (=U0TXD) is left driven as an output by the bootloader, which
    // fights the SP3485's RO and kills our RX. Reset both pins to a
    // clean GPIO state so uart_set_pin can route them to UART1 via the
    // GPIO matrix with no IOMUX/UART0 contention. (Without this, TX
    // works but RX sees nothing -- GPIO44=U0RXD is only an input, so
    // it was never contended.)
    gpio_reset_pin((gpio_num_t)RS485_PIN_RX);
    gpio_reset_pin((gpio_num_t)RS485_PIN_TX);
    // No DE/RTS pin: the Waveshare 4.3B RS485 is auto-direction (TX
    // line drives DE/RE in hardware), so we run plain UART mode.
    err = uart_set_pin(UART_NUM, RS485_PIN_TX, RS485_PIN_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) { log_e("uart_set_pin: 0x%x",         err); return false; }
    err = uart_set_mode(UART_NUM, UART_MODE_UART);
    if (err != ESP_OK) { log_e("uart_set_mode: 0x%x",        err); return false; }
    // Slightly tighter RX timeout (in baud-time units) gets us out of
    // the FIFO drain sooner during back-to-back transactions.
    uart_set_rx_timeout(UART_NUM, 3);
    return true;
}

static size_t uart_write_blocking(const uint8_t* buf, size_t len) {
    int n = uart_write_bytes(UART_NUM, (const char*)buf, len);
    if (n < 0) return 0;
    // Block until DE drops, so we know the line is back to listen.
    uart_wait_tx_done(UART_NUM, pdMS_TO_TICKS(100));
    return (size_t)n;
}

// Drain anything currently in the RX FIFO. Call before a new
// transaction to discard stale bytes from a previous failed exchange.
static void uart_drain_rx() {
    uart_flush_input(UART_NUM);
    decoder_reset(s_decoder);
}

// ===================================================================
// One transact() attempt: send, then read until decoded frame or
// deadline. Returns true if a matching response was received.
// ===================================================================
static bool transact_attempt(const uint8_t* frame, size_t frame_len,
                             uint32_t timeout_ms) {
    uart_drain_rx();
    s_rx.have_frame = false;

    uart_write_blocking(frame, frame_len);

    const uint32_t deadline = millis() + timeout_ms;
    uint8_t rx[64];
    while (millis() < deadline) {
        int got = uart_read_bytes(UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(5));
        if (got > 0) {
            for (int i = 0; i < got; ++i) decoder_feed(s_decoder, rx[i]);
            if (s_rx.have_frame) return true;
        }
    }
    return false;
}

// ===================================================================
// Public transact()
// ===================================================================
Status transact(uint8_t addr, uint8_t type,
                const uint8_t* payload, size_t payload_len,
                uint8_t* out_payload, size_t out_payload_cap,
                size_t*  out_payload_len,
                uint32_t timeout_ms, uint8_t retries,
                uint8_t* out_err_code) {
    if (!s_inited) return Status::NotReady;
    if (payload_len > MAX_PAYLOAD) return Status::BufferTooSmall;

    static uint8_t tx_buf[MAX_FRAME_BYTES];
    Status result = Status::Timeout;

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return Status::Internal;
    }

    for (uint8_t attempt = 0; attempt <= retries; ++attempt) {
        const uint8_t seq = s_next_seq++;
        if (s_next_seq == 0) s_next_seq = 1;   // 0 reserved
        s_rx.match_seq = seq;

        size_t n = encode_frame(tx_buf, sizeof(tx_buf),
                                addr, seq, type, payload, payload_len);
        if (n == 0) { result = Status::Internal; break; }

        s_stats.tx_frames++;
        if (transact_attempt(tx_buf, n, timeout_ms)) {
            s_stats.rx_frames++;

            // Did the slave return an error response?
            if (is_error(s_rx.type)) {
                if (out_err_code && s_rx.payload_len >= 1) {
                    *out_err_code = s_rx.payload[0];
                }
                result = Status::BadResponse;
                break;
            }
            // Expected response type is request | 0x80.
            if (s_rx.type != mk_response(type)) {
                result = Status::BadResponse;
                break;
            }

            // Copy payload back to caller.
            if (out_payload && out_payload_cap < s_rx.payload_len) {
                result = Status::BufferTooSmall;
                break;
            }
            if (out_payload && s_rx.payload_len) {
                memcpy(out_payload, s_rx.payload, s_rx.payload_len);
            }
            if (out_payload_len) *out_payload_len = s_rx.payload_len;
            result = Status::Ok;
            break;
        }
        s_stats.timeouts++;
        if (attempt < retries) s_stats.retries++;
    }

    xSemaphoreGive(s_bus_mutex);
    return result;
}

// ===================================================================
// POLL task
// ===================================================================
static void dispatch_events_from_poll(const uint8_t* p, size_t len) {
    if (len < 1) return;
    uint8_t count = p[0];
    p   += 1;
    len -= 1;

    // Collect the SEQs to ACK while we walk the list.
    uint8_t ack_seqs[32];
    int     n_acks = 0;

    for (uint8_t i = 0; i < count && len >= 3; ++i) {
        const uint8_t e_type = p[0];
        const uint8_t e_seq  = p[1];
        const uint8_t e_len  = p[2];
        if (3u + e_len > len) {
            log_w("rs485: malformed event payload (len overrun)");
            break;
        }
        const uint8_t* e_data = (e_len > 0) ? &p[3] : nullptr;

        if (s_event_cb) {
            Event ev{ e_type, e_seq, e_data, e_len };
            s_event_cb(ev, s_event_user);
        }
        if (n_acks < (int)sizeof(ack_seqs)) ack_seqs[n_acks++] = e_seq;
        s_stats.events_received++;

        p   += 3 + e_len;
        len -= 3 + e_len;
    }

    if (n_acks > 0) {
        // Send ACK_EVENTS: count byte + SEQs. Fire-and-forget with
        // 0 retries; if the Core misses it the events resend on the
        // next POLL.
        uint8_t pkt[1 + sizeof(ack_seqs)];
        pkt[0] = (uint8_t)n_acks;
        memcpy(&pkt[1], ack_seqs, n_acks);
        transact(ADDR_CORE, MSG_ACK_EVENTS,
                 pkt, 1 + n_acks,
                 nullptr, 0, nullptr,
                 DEFAULT_TIMEOUT_MS, /*retries=*/0);
    }
}

static void poll_task(void* /*arg*/) {
    constexpr TickType_t period = pdMS_TO_TICKS(POLL_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();
    uint8_t  buf[MAX_PAYLOAD];
    size_t   plen = 0;

    for (;;) {
        if (!s_poll_paused) {
            Status s = transact(ADDR_CORE, MSG_POLL,
                                 nullptr, 0,
                                 buf, sizeof(buf), &plen,
                                 DEFAULT_TIMEOUT_MS, /*retries=*/0);
            if (s == Status::Ok && plen > 0) {
                dispatch_events_from_poll(buf, plen);
            }
        }
        vTaskDelayUntil(&last, period);
    }
}

// ===================================================================
// Public init + housekeeping
// ===================================================================
Status init() {
    if (s_inited) return Status::Ok;

    s_bus_mutex = xSemaphoreCreateMutex();
    if (!s_bus_mutex) return Status::Internal;

    if (!uart_setup()) return Status::Internal;

    s_decoder = decoder_create(on_decoded_frame, nullptr);
    if (!s_decoder) return Status::Internal;

    s_inited = true;

    // Lower priority than the LVGL task (2) so UI stays smooth even
    // under heavy bus chatter.
    xTaskCreatePinnedToCore(poll_task, "rs485-poll",
                            4 * 1024, nullptr,
                            /*priority=*/1, &s_poll_task,
                            PRO_CPU_NUM);
    return Status::Ok;
}

void set_event_handler(EventHandler cb, void* user) {
    s_event_cb   = cb;
    s_event_user = user;
}

const Stats& stats() { return s_stats; }

void poll_pause()  { s_poll_paused = true; }
void poll_resume() { s_poll_paused = false; }

size_t debug_raw_listen(uint32_t ms, uint8_t* sample, size_t sample_cap,
                        size_t* sample_len) {
    if (!s_inited) return 0;
    poll_pause();
    vTaskDelay(pdMS_TO_TICKS(5));          // let an in-flight poll finish
    uart_flush_input(UART_NUM);
    size_t   count = 0, sn = 0;
    uint8_t  rx[128];
    const uint32_t deadline = millis() + ms;
    while ((int32_t)(millis() - deadline) < 0) {
        int got = uart_read_bytes(UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(10));
        if (got > 0) {
            count += got;
            for (int i = 0; i < got && sn < sample_cap; ++i) sample[sn++] = rx[i];
        }
    }
    poll_resume();
    if (sample_len) *sample_len = sn;
    return count;
}

// ===================================================================
// High-level wrappers
// ===================================================================
Status ping(uint8_t addr) {
    return transact(addr, MSG_PING, nullptr, 0, nullptr, 0, nullptr);
}

Status get_version(CoreVersion& out, uint8_t addr) {
    uint8_t buf[16];
    size_t  n = 0;
    Status s = transact(addr, MSG_GET_VERSION, nullptr, 0, buf, sizeof(buf), &n);
    if (s != Status::Ok)  return s;
    if (n < 8)            return Status::BadResponse;
    out.fw_major = buf[0];
    out.fw_minor = buf[1];
    out.fw_patch = buf[2];
    out.build_id = ((uint32_t)buf[3] << 24) | ((uint32_t)buf[4] << 16) |
                   ((uint32_t)buf[5] <<  8) | ((uint32_t)buf[6] <<  0);
    out.hw_rev   = buf[7];
    return Status::Ok;
}

Status get_status(CoreStatus& out, uint8_t addr) {
    uint8_t buf[16];
    size_t  n = 0;
    Status s = transact(addr, MSG_GET_STATUS, nullptr, 0, buf, sizeof(buf), &n);
    if (s != Status::Ok)  return s;
    if (n < 7)            return Status::BadResponse;
    out.uptime_s     = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                       ((uint32_t)buf[2] <<  8) | ((uint32_t)buf[3] <<  0);
    out.reels_present = buf[4];
    out.error_flags   = ((uint16_t)buf[5] << 8) | buf[6];
    return Status::Ok;
}

Status reset_core(ResetReason reason, uint8_t addr) {
    uint8_t reason_b = (uint8_t)reason;
    // The Core resets immediately after ACKing, so retry once and
    // accept a Timeout on the second attempt as success.
    return transact(addr, MSG_RESET, &reason_b, 1,
                    nullptr, 0, nullptr,
                    DEFAULT_TIMEOUT_MS, /*retries=*/0);
}

// ---- Reel I/O ------------------------------------------------------
Status get_reel_info(uint8_t reel_id, ReelInfo* infos, int max_infos, int* n_out,
                     uint8_t addr) {
    if (!infos || max_infos <= 0) return Status::BufferTooSmall;
    uint8_t buf[256];
    size_t  n = 0;
    Status s = transact(addr, MSG_GET_REEL_INFO, &reel_id, 1, buf, sizeof(buf), &n);
    if (s != Status::Ok) return s;

    // Response layout: count (1B) + per-reel record(s):
    //   reel_id (1B), present (1B), sense_mv (2B BE),
    //   id_len (1B), id_bytes (id_len B)
    if (n < 1) return Status::BadResponse;
    int count = buf[0];
    const uint8_t* p = &buf[1];
    int remaining = (int)n - 1;
    int written = 0;
    for (int i = 0; i < count && remaining >= 5 && written < max_infos; ++i) {
        const uint8_t wire_id_len = p[4];
        const int record_bytes = 5 + wire_id_len;
        if (record_bytes > remaining) break;

        ReelInfo& r = infos[written];
        // p[0] = reel_id echo; we expose the array index instead.
        r.present  = (p[1] != 0);
        r.sense_mv = ((uint16_t)p[2] << 8) | p[3];
        uint8_t copy_len = wire_id_len;
        if (copy_len > sizeof(r.id_bytes)) copy_len = sizeof(r.id_bytes);
        memcpy(r.id_bytes, &p[5], copy_len);
        r.id_len = copy_len;

        p         += record_bytes;
        remaining -= record_bytes;
        written++;
    }
    if (n_out) *n_out = written;
    return Status::Ok;
}

Status read_inputs(uint8_t reel_id, uint8_t* out, size_t out_cap, size_t* out_len,
                   uint8_t addr) {
    if (!out || !out_cap) return Status::BufferTooSmall;
    size_t n = 0;
    Status s = transact(addr, MSG_READ_INPUTS, &reel_id, 1, out, out_cap, &n);
    if (out_len) *out_len = n;
    return s;
}

Status set_poll_rate(uint8_t reel_id, uint16_t rate_hz, uint8_t addr) {
    uint8_t p[3] = { reel_id, (uint8_t)(rate_hz >> 8), (uint8_t)(rate_hz & 0xFF) };
    return transact(addr, MSG_SET_POLL_RATE, p, sizeof(p), nullptr, 0, nullptr);
}

// ---- LED control ---------------------------------------------------
Status set_reel_pixels(uint8_t reel_id, uint16_t start_idx,
                       const uint8_t* rgb_triples, size_t triple_count,
                       uint8_t addr) {
    if (triple_count == 0) return Status::Ok;
    // Payload: reel_id, start_h, start_l, count, R,G,B,...
    static uint8_t buf[MAX_PAYLOAD];
    const size_t bytes = triple_count * 3;
    if (4 + bytes > sizeof(buf)) return Status::BufferTooSmall;
    buf[0] = reel_id;
    buf[1] = (uint8_t)(start_idx >> 8);
    buf[2] = (uint8_t)(start_idx & 0xFF);
    buf[3] = (uint8_t)triple_count;
    memcpy(&buf[4], rgb_triples, bytes);
    return transact(addr, MSG_SET_REEL_PIXELS, buf, 4 + bytes, nullptr, 0, nullptr);
}

Status fill_reel(uint8_t reel_id, uint8_t r, uint8_t g, uint8_t b, uint8_t addr) {
    uint8_t p[4] = { reel_id, r, g, b };
    return transact(addr, MSG_FILL_REEL, p, sizeof(p), nullptr, 0, nullptr);
}

Status set_brightness(uint8_t reel_id, uint8_t brightness, uint8_t addr) {
    uint8_t p[2] = { reel_id, brightness };
    return transact(addr, MSG_SET_BRIGHTNESS, p, sizeof(p), nullptr, 0, nullptr);
}

Status set_animation(uint8_t reel_id, uint8_t anim_id,
                     const uint8_t* params, size_t params_len,
                     uint8_t addr) {
    static uint8_t buf[64];
    if (2 + params_len > sizeof(buf)) return Status::BufferTooSmall;
    buf[0] = reel_id;
    buf[1] = anim_id;
    if (params_len) memcpy(&buf[2], params, params_len);
    return transact(addr, MSG_SET_ANIMATION, buf, 2 + params_len, nullptr, 0, nullptr);
}

Status commit_pixels(uint8_t reel_bitmap, uint8_t addr) {
    uint8_t p = reel_bitmap;
    return transact(addr, MSG_COMMIT, &p, 1, nullptr, 0, nullptr);
}

} // namespace rs485
