// =====================================================================
//  SmartReel Core -- production firmware (RP2040)
//
//  The real core firmware. It drives the Core PCB's slot hardware
//  (WS2812B LEDs, ADS1115 reel-sense, 74HC165 inputs -- see reel_hw.*)
//  and speaks the slave half of docs/smartreel-rs485-protocol.md to the
//  ESP32 HMI bus master.
//
//  Hardware model (see config.h): four PORTS, each carrying 1..4 chained
//  reel MODULES (16 LEDs + 32 input bits each). Module count per port is
//  sensed from a voltage divider.
//
//  Loop structure: service the RS485 link first (replies sent inline as
//  each request decodes), then run the input sampler (~100 Hz) and the
//  sense sampler (round-robin), then refresh the debug LEDs. No blocking
//  calls on the hot path, so request latency stays under the HMI's
//  ~50 ms transaction timeout. Hardware sampling is suspended during a
//  firmware update (flash writes disable XIP / interrupts).
//
//  Serial   -- USB CDC operator/diagnostic console (type 'help').
//  Serial1  -- UART0, the SP3485 RS485 link to the HMI.
// =====================================================================

#include <Arduino.h>
#include <Updater.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "reel_hw.h"
#include "rs485/rs485_proto.h"
#include "rs485/rs485_frame.h"
#include "sha256.h"
#include "fw_tag.h"

using namespace rs485;

// Semantic version comes from -DFW_VER_* (platformio.ini). The embedded
// tag lets the HMI read this version out of core.bin on the SD card.
FW_TAG_DEFINE("core");

#define CORE_BUILD_TAG "prod"
#define RS485 Serial1

// ---- Diagnostics ---------------------------------------------------
static uint32_t s_boot_ms = 0;
static uint16_t s_error_flags = 0;
static uint32_t s_rx_frames = 0, s_tx_frames = 0, s_polls = 0;
static uint32_t s_rx_bytes  = 0;
static uint32_t s_last_master_ms = 0;   // last valid frame addressed to us

// ---- Async event ring ----------------------------------------------
// Events live here until the HMI ACKs their SEQ (MSG_ACK_EVENTS). Until
// then every POLL re-reports them -- the resend behaviour the protocol
// doc calls for.
struct PendingEvt {
    bool    used;
    uint8_t seq;
    uint8_t type;
    uint8_t len;
    uint8_t data[64];
};
static constexpr int EVT_RING = 32;
static PendingEvt s_evt[EVT_RING];
static uint8_t    s_evt_seq = 1;   // 0 is reserved

static uint8_t next_evt_seq() {
    uint8_t s = s_evt_seq++;
    if (s_evt_seq == 0) s_evt_seq = 1;
    return s;
}

static bool enqueue_event(uint8_t type, const uint8_t* data, uint8_t len) {
    if (len > sizeof(((PendingEvt*)0)->data)) len = sizeof(((PendingEvt*)0)->data);
    for (int i = 0; i < EVT_RING; ++i) {
        if (!s_evt[i].used) {
            s_evt[i].used = true;
            s_evt[i].seq  = next_evt_seq();
            s_evt[i].type = type;
            s_evt[i].len  = len;
            if (len) memcpy(s_evt[i].data, data, len);
            return true;
        }
    }
    Serial.println("[evt] ring full -- event dropped (HMI not ACKing?)");
    return false;
}

static void ack_event_seq(uint8_t seq) {
    for (int i = 0; i < EVT_RING; ++i)
        if (s_evt[i].used && s_evt[i].seq == seq) { s_evt[i].used = false; return; }
}

// ---- byte helpers --------------------------------------------------
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put_be16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }

// ---- Debug LED activity blip ---------------------------------------
static uint32_t s_aux_until = 0;
static inline void aux_blip(uint32_t ms = 40) { s_aux_until = millis() + ms; }

// =====================================================================
//  Hardware -> RS485 event bridge (callbacks from reel_hw sampling)
// =====================================================================
static void on_input_change(uint8_t port, uint8_t module,
                            uint32_t prev, uint32_t now) {
    uint8_t d[14];
    d[0] = port;
    d[1] = module;
    put_be32(&d[2],  prev);
    put_be32(&d[6],  now);
    put_be32(&d[10], millis());
    enqueue_event(EVT_INPUT_CHANGE, d, sizeof(d));
    aux_blip();
}

static void on_module_count(uint8_t port, uint8_t prev_count,
                            uint8_t now_count, uint16_t sense_mv) {
    if (now_count > prev_count) {
        uint8_t d[4] = { port, now_count, 0, 0 };
        put_be16(&d[2], sense_mv);
        enqueue_event(EVT_REEL_INSERTED, d, sizeof(d));
    } else if (now_count < prev_count) {
        uint8_t d[2] = { port, now_count };
        enqueue_event(EVT_REEL_REMOVED, d, sizeof(d));
    }
    aux_blip();
}

// =====================================================================
//  RS485 transmit (half-duplex turnaround)
// =====================================================================
static uint8_t s_txbuf[MAX_FRAME_BYTES];

static void rs485_send(uint8_t type, uint8_t seq,
                       const uint8_t* payload, size_t plen) {
    size_t n = encode_frame(s_txbuf, sizeof(s_txbuf),
                            ADDR_MASTER, seq, type, payload, plen);
    if (!n) return;
    // Turnaround: the HMI's auto-direction transceiver needs a moment to
    // switch drive->receive after sending. Reply too soon and it misses
    // the start of our frame (master-side timeout, no CRC error).
    delayMicroseconds(500);
    digitalWrite(cfg::PIN_RS485_DE, HIGH);
    RS485.write(s_txbuf, n);
    RS485.flush();                  // drains TX FIFO (not the shift reg)
    // Hold DE until the final byte fully shifts out: 10 bits/115200 ~= 87us.
    delayMicroseconds(200);
    digitalWrite(cfg::PIN_RS485_DE, LOW);
    s_tx_frames++;
}

static void send_ack(uint8_t req_type, uint8_t seq) {
    rs485_send(mk_response(req_type), seq, nullptr, 0);
}
static void send_error(uint8_t req_type, uint8_t seq, uint8_t err) {
    rs485_send(mk_error_response(req_type), seq, &err, 1);
}

// =====================================================================
//  Request handlers (slave side)
// =====================================================================
static void handle_poll(uint8_t seq) {
    s_polls++;
    uint8_t buf[MAX_PAYLOAD];
    size_t  off = 1;            // reserve byte 0 for count
    uint8_t count = 0;
    for (int i = 0; i < EVT_RING; ++i) {
        if (!s_evt[i].used) continue;
        const size_t need = 3 + s_evt[i].len;
        if (off + need > sizeof(buf)) break;
        buf[off++] = s_evt[i].type;
        buf[off++] = s_evt[i].seq;
        buf[off++] = s_evt[i].len;
        if (s_evt[i].len) { memcpy(&buf[off], s_evt[i].data, s_evt[i].len); off += s_evt[i].len; }
        count++;
    }
    buf[0] = count;
    rs485_send(mk_response(MSG_POLL), seq, buf, off);
}

static void handle_ack_events(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 1) { send_error(MSG_ACK_EVENTS, seq, ERR_BAD_PAYLOAD); return; }
    uint8_t n = p[0];
    for (uint8_t i = 0; i < n && (size_t)(1 + i) < len; ++i) ack_event_seq(p[1 + i]);
    send_ack(MSG_ACK_EVENTS, seq);
}

static void handle_get_version(uint8_t seq) {
    const uint8_t v[8] = { g_fw_tag.major, g_fw_tag.minor, g_fw_tag.patch,
                           0, 0, 0, 0, 0x01 /*hw_rev*/ };
    rs485_send(mk_response(MSG_GET_VERSION), seq, v, sizeof(v));
}

static void handle_get_status(uint8_t seq) {
    uint32_t up = (millis() - s_boot_ms) / 1000;
    uint8_t v[7];
    put_be32(&v[0], up);
    v[4] = reel_hw::reels_present_bitmap();
    put_be16(&v[5], s_error_flags);
    rs485_send(mk_response(MSG_GET_STATUS), seq, v, sizeof(v));
}

// One GET_REEL_INFO record: port, present, sense_mv(2 BE), module_count.
static void append_reel_record(uint8_t* buf, size_t& off, uint8_t p) {
    const reel_hw::PortState& st = reel_hw::port(p);
    buf[off++] = p;
    buf[off++] = st.module_count > 0 ? 1 : 0;
    put_be16(&buf[off], st.sense_mv); off += 2;
    buf[off++] = st.module_count;
}

static void handle_get_reel_info(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 1) { send_error(MSG_GET_REEL_INFO, seq, ERR_BAD_PAYLOAD); return; }
    uint8_t port = p[0];
    uint8_t buf[MAX_PAYLOAD];
    size_t  off = 1;
    uint8_t count = 0;
    if (port == REEL_ID_ALL) {
        for (uint8_t r = 0; r < cfg::N_PORTS; ++r) { append_reel_record(buf, off, r); count++; }
    } else if (port < cfg::N_PORTS) {
        append_reel_record(buf, off, port); count = 1;
    } else {
        send_error(MSG_GET_REEL_INFO, seq, ERR_REEL_ABSENT); return;
    }
    buf[0] = count;
    rs485_send(mk_response(MSG_GET_REEL_INFO), seq, buf, off);
}

// READ_INPUTS(port): port, module_count, {u32 BE} x module_count.
static void handle_read_inputs(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 1) { send_error(MSG_READ_INPUTS, seq, ERR_BAD_PAYLOAD); return; }
    uint8_t port = p[0];
    if (port >= cfg::N_PORTS) { send_error(MSG_READ_INPUTS, seq, ERR_REEL_ABSENT); return; }
    const reel_hw::PortState& st = reel_hw::port(port);
    uint8_t buf[2 + cfg::MAX_MODULES_PER_PORT * 4];
    size_t  off = 0;
    buf[off++] = port;
    buf[off++] = st.module_count;
    for (uint8_t m = 0; m < st.module_count && m < cfg::MAX_MODULES_PER_PORT; ++m) {
        put_be32(&buf[off], st.inputs[m]); off += 4;
    }
    rs485_send(mk_response(MSG_READ_INPUTS), seq, buf, off);
}

// ---- LED control ---------------------------------------------------
// SET_REEL_PIXELS stages into the back buffer (atomic multi-port shows
// land on COMMIT). FILL_REEL and SET_BRIGHTNESS apply immediately --
// they're commonly used standalone -- by committing the affected port.
static void handle_set_reel_pixels(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 4) { send_error(MSG_SET_REEL_PIXELS, seq, ERR_BAD_PAYLOAD); return; }
    uint8_t  port  = p[0];
    uint16_t start = ((uint16_t)p[1] << 8) | p[2];
    uint8_t  cnt   = p[3];
    if (port >= cfg::N_PORTS) { send_error(MSG_SET_REEL_PIXELS, seq, ERR_REEL_ABSENT); return; }
    if (len < (size_t)(4 + cnt * 3)) { send_error(MSG_SET_REEL_PIXELS, seq, ERR_BAD_PAYLOAD); return; }
    reel_hw::stage_pixels(port, start, &p[4], cnt);
    send_ack(MSG_SET_REEL_PIXELS, seq);
}

static void handle_fill_reel(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 4) { send_error(MSG_FILL_REEL, seq, ERR_BAD_PAYLOAD); return; }
    uint8_t port = p[0];
    if (port >= cfg::N_PORTS) { send_error(MSG_FILL_REEL, seq, ERR_REEL_ABSENT); return; }
    reel_hw::stage_fill(port, p[1], p[2], p[3]);
    reel_hw::commit(1u << port);
    send_ack(MSG_FILL_REEL, seq);
}

static void handle_set_brightness(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 2) { send_error(MSG_SET_BRIGHTNESS, seq, ERR_BAD_PAYLOAD); return; }
    uint8_t port = p[0];
    if (port != REEL_ID_ALL && port >= cfg::N_PORTS) {
        send_error(MSG_SET_BRIGHTNESS, seq, ERR_REEL_ABSENT); return;
    }
    reel_hw::set_brightness(port, p[1]);
    uint8_t bitmap = (port == REEL_ID_ALL) ? ((1u << cfg::N_PORTS) - 1) : (1u << port);
    reel_hw::commit(bitmap);
    send_ack(MSG_SET_BRIGHTNESS, seq);
}

static void handle_commit(uint8_t seq, const uint8_t* p, size_t len) {
    uint8_t bitmap = (len >= 1) ? p[0] : ((1u << cfg::N_PORTS) - 1);
    reel_hw::commit(bitmap);
    send_ack(MSG_COMMIT, seq);
}

// =====================================================================
//  Firmware update receiver (arduino-pico OTA staging, validated)
// =====================================================================
static bool       s_fw_active   = false;
static uint32_t   s_fw_total    = 0;
static uint32_t   s_fw_received = 0;
static sha256_ctx s_fw_sha;
static uint8_t    s_fw_expected[32];

static void handle_fw_begin(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 40) { send_error(MSG_FW_BEGIN, seq, ERR_BAD_PAYLOAD); return; }
    s_fw_total = be32(p);
    memcpy(s_fw_expected, p + 4, 32);
    uint32_t version = be32(p + 36);
    if (!Update.begin(s_fw_total, U_FLASH)) {
        Serial.printf("[fw] Update.begin failed (size=%lu)\n", (unsigned long)s_fw_total);
        send_error(MSG_FW_BEGIN, seq, ERR_BUSY);
        s_fw_active = false;
        return;
    }
    sha256_init(&s_fw_sha);
    s_fw_received = 0;
    s_fw_active   = true;
    Serial.printf("[fw] BEGIN size=%lu version=0x%08lX -- staging\n",
                  (unsigned long)s_fw_total, (unsigned long)version);
    send_ack(MSG_FW_BEGIN, seq);
}

static void handle_fw_chunk(uint8_t seq, const uint8_t* p, size_t len) {
    if (!s_fw_active || len < 4) { send_error(MSG_FW_CHUNK, seq, ERR_FW_STATE); return; }
    uint32_t offset = be32(p);
    const uint8_t* data = p + 4;
    size_t dlen = len - 4;
    if (offset == s_fw_received) {
        if (Update.write((uint8_t*)data, dlen) != dlen) {
            Serial.println("[fw] write failed");
            send_error(MSG_FW_CHUNK, seq, ERR_BUSY);
            return;
        }
        sha256_update(&s_fw_sha, data, dlen);
        s_fw_received += dlen;
    } else if (offset + dlen <= s_fw_received) {
        // already have it (retransmit) -- re-ACK
    } else {
        send_error(MSG_FW_CHUNK, seq, ERR_FW_OOR);
        return;
    }
    uint8_t ack[4]; put_be32(ack, s_fw_received);
    rs485_send(mk_response(MSG_FW_CHUNK), seq, ack, sizeof(ack));
}

static void handle_fw_verify(uint8_t seq) {
    if (!s_fw_active) { send_error(MSG_FW_VERIFY, seq, ERR_FW_STATE); return; }
    uint8_t got[32];
    sha256_ctx tmp = s_fw_sha;
    sha256_final(&tmp, got);
    if (s_fw_received != s_fw_total || memcmp(got, s_fw_expected, 32) != 0) {
        Serial.printf("[fw] VERIFY FAILED (received=%lu/%lu, hash %s)\n",
                      (unsigned long)s_fw_received, (unsigned long)s_fw_total,
                      memcmp(got, s_fw_expected, 32) ? "mismatch" : "ok");
        send_error(MSG_FW_VERIFY, seq, ERR_FW_HASH);
        return;
    }
    Serial.println("[fw] VERIFY ok");
    send_ack(MSG_FW_VERIFY, seq);
}

static void handle_fw_commit(uint8_t seq) {
    if (!s_fw_active) { send_error(MSG_FW_COMMIT, seq, ERR_FW_STATE); return; }
    if (!Update.end(true)) {
        Serial.println("[fw] Update.end failed");
        send_error(MSG_FW_COMMIT, seq, ERR_BUSY);
        return;
    }
    s_fw_active = false;
    Serial.println("[fw] COMMIT ok -- rebooting into new image");
    send_ack(MSG_FW_COMMIT, seq);
    delay(50);            // let the ACK flush onto the bus
    rp2040.reboot();      // OTA bootloader flashes the staged image
}

// =====================================================================
//  Frame dispatch
// =====================================================================
static void on_frame(uint8_t addr, uint8_t seq, uint8_t type,
                     const uint8_t* payload, size_t payload_len,
                     void* /*user*/) {
    if (addr != ADDR_CORE) return;
    if (is_response(type))  return;
    s_rx_frames++;
    s_last_master_ms = millis();

    switch (type) {
        case MSG_PING:          send_ack(MSG_PING, seq);                 break;
        case MSG_GET_VERSION:   handle_get_version(seq);                 break;
        case MSG_GET_STATUS:    handle_get_status(seq);                  break;
        case MSG_POLL:          handle_poll(seq);                        break;
        case MSG_ACK_EVENTS:    handle_ack_events(seq, payload, payload_len); break;
        case MSG_RESET: {
            uint8_t reason = payload_len ? payload[0] : 0;
            Serial.printf("[rs485] RESET reason=%u\n", reason);
            send_ack(MSG_RESET, seq);
            if (reason == 0) { delay(50); rp2040.reboot(); }
            // reason 1 (enter_update_mode): app handles FW_* live, no reboot.
            break;
        }
        case MSG_GET_REEL_INFO: handle_get_reel_info(seq, payload, payload_len); break;
        case MSG_READ_INPUTS:   handle_read_inputs(seq, payload, payload_len);   break;
        case MSG_SET_POLL_RATE: send_ack(MSG_SET_POLL_RATE, seq);        break;  // fixed-rate sampler
        case MSG_SET_REEL_PIXELS: handle_set_reel_pixels(seq, payload, payload_len); break;
        case MSG_FILL_REEL:       handle_fill_reel(seq, payload, payload_len);       break;
        case MSG_SET_BRIGHTNESS:  handle_set_brightness(seq, payload, payload_len);  break;
        case MSG_SET_ANIMATION:   send_ack(MSG_SET_ANIMATION, seq);      break;  // deferred (ACK only)
        case MSG_COMMIT:          handle_commit(seq, payload, payload_len);          break;
        case MSG_FW_BEGIN:        handle_fw_begin(seq, payload, payload_len);  break;
        case MSG_FW_CHUNK:        handle_fw_chunk(seq, payload, payload_len);  break;
        case MSG_FW_VERIFY:       handle_fw_verify(seq);                       break;
        case MSG_FW_COMMIT:       handle_fw_commit(seq);                       break;
        default:
            Serial.printf("[rs485] unknown cmd 0x%02X\n", type);
            send_error(type, seq, ERR_UNKNOWN_CMD);
            break;
    }
}

static Decoder* s_decoder = nullptr;

// =====================================================================
//  USB diagnostic console
// =====================================================================
static void print_help() {
    Serial.println(F(
        "\nSmartReel Core (production) -- diagnostic console:\n"
        "  help              this text\n"
        "  ver               firmware version\n"
        "  status            per-port module count / sense / inputs\n"
        "  stats             RS485 frame counters + link state\n"
        "  scan              force a sense + input sample now\n"
        "  on  <port> <hex>  fill a port's LEDs 0xRRGGBB (port 0-3) + show\n"
        "  off [port]        all LEDs off, or one port\n"
        "  bright <port|255> <0-255>  set brightness (255 = all)\n"
        "Reel inputs/sense are driven by real hardware; the HMI is the\n"
        "RS485 master. Numbers accept 0x.. hex or decimal."));
}

static long tok_num(const char* t, long def) {
    if (!t || !*t) return def;
    return strtol(t, nullptr, 0);
}

static void print_status() {
    Serial.printf("uptime=%lus  present=0x%02X  errflags=0x%04X  ads=%s\n",
                  (unsigned long)((millis() - s_boot_ms) / 1000),
                  reel_hw::reels_present_bitmap(), s_error_flags,
                  reel_hw::ads_ok() ? "ok" : "MISSING");
    for (uint8_t p = 0; p < cfg::N_PORTS; ++p) {
        const reel_hw::PortState& st = reel_hw::port(p);
        Serial.printf("  port %u: %u module(s)  sense=%4u mV", p, st.module_count, st.sense_mv);
        for (uint8_t m = 0; m < st.module_count; ++m)
            Serial.printf("  m%u=0x%08lX", m, (unsigned long)st.inputs[m]);
        Serial.println();
    }
    int pending = 0;
    for (int i = 0; i < EVT_RING; ++i) if (s_evt[i].used) pending++;
    Serial.printf("  pending (un-ACKed) events: %d\n", pending);
}

static void process_console_line(char* line) {
    char* cmd = strtok(line, " \t");
    if (!cmd) return;
    if (!strcmp(cmd, "help")) { print_help(); return; }
    if (!strcmp(cmd, "ver")) {
        Serial.printf("v%u.%u.%u  built %s  (%s)\n",
                      g_fw_tag.major, g_fw_tag.minor, g_fw_tag.patch,
                      g_fw_tag.build, CORE_BUILD_TAG);
        return;
    }
    if (!strcmp(cmd, "status")) { print_status(); return; }
    if (!strcmp(cmd, "stats")) {
        Serial.printf("rx_bytes=%lu rx_frames=%lu tx_frames=%lu polls=%lu link=%s\n",
                      (unsigned long)s_rx_bytes, (unsigned long)s_rx_frames,
                      (unsigned long)s_tx_frames, (unsigned long)s_polls,
                      (millis() - s_last_master_ms) < cfg::LINK_TIMEOUT_MS ? "UP" : "down");
        return;
    }
    if (!strcmp(cmd, "scan")) { reel_hw::sample_sense(); reel_hw::sample_inputs(); print_status(); return; }
    if (!strcmp(cmd, "on")) {
        int port = (int)tok_num(strtok(nullptr, " \t"), -1);
        long rgb = tok_num(strtok(nullptr, " \t"), 0xFFFFFF);
        if (port < 0 || port >= cfg::N_PORTS) { Serial.println(F("usage: on <port 0-3> <0xRRGGBB>")); return; }
        reel_hw::stage_fill(port, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        reel_hw::commit(1u << port);
        Serial.printf("[led] port %d -> 0x%06lX\n", port, (unsigned long)(rgb & 0xFFFFFF));
        return;
    }
    if (!strcmp(cmd, "off")) {
        char* a = strtok(nullptr, " \t");
        if (!a) {
            for (uint8_t p = 0; p < cfg::N_PORTS; ++p) reel_hw::stage_fill(p, 0, 0, 0);
            reel_hw::commit((1u << cfg::N_PORTS) - 1);
            Serial.println(F("[led] all off"));
            return;
        }
        int port = (int)tok_num(a, -1);
        if (port < 0 || port >= cfg::N_PORTS) { Serial.println(F("usage: off [port 0-3]")); return; }
        reel_hw::stage_fill(port, 0, 0, 0);
        reel_hw::commit(1u << port);
        Serial.printf("[led] port %d off\n", port);
        return;
    }
    if (!strcmp(cmd, "bright")) {
        int port = (int)tok_num(strtok(nullptr, " \t"), 255);
        int bri  = (int)tok_num(strtok(nullptr, " \t"), -1);
        if (bri < 0 || bri > 255) { Serial.println(F("usage: bright <port|255> <0-255>")); return; }
        reel_hw::set_brightness((uint8_t)port, (uint8_t)bri);
        reel_hw::commit((port == 255) ? ((1u << cfg::N_PORTS) - 1) : (1u << port));
        Serial.printf("[led] brightness port=%d -> %d\n", port, bri);
        return;
    }
    Serial.printf("unknown command '%s' -- type 'help'\n", cmd);
}

static void poll_console() {
    static char buf[96];
    static size_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') { buf[len] = 0; if (len) process_console_line(buf); len = 0; continue; }
        if (len < sizeof(buf) - 1) buf[len++] = c;
    }
}

// =====================================================================
//  Debug LEDs
//   LINK (GP2): solid when a master frame arrived within LINK_TIMEOUT_MS,
//               otherwise a slow blink to flag "no HMI".
//   AUX  (GP3): brief blip on reel activity (input / insert / remove).
// =====================================================================
static void update_debug_leds() {
    uint32_t now = millis();
    bool link_up = (now - s_last_master_ms) < cfg::LINK_TIMEOUT_MS;
    bool link_on = link_up ? true : ((now / 250) & 1);   // solid vs ~2 Hz blink
    digitalWrite(cfg::PIN_DBG_LINK, link_on ? HIGH : LOW);
    digitalWrite(cfg::PIN_DBG_AUX, (now < s_aux_until) ? HIGH : LOW);
}

// =====================================================================
//  Arduino entry points
// =====================================================================
void setup() {
    Serial.begin(115200);

    pinMode(cfg::PIN_DBG_LINK, OUTPUT); digitalWrite(cfg::PIN_DBG_LINK, LOW);
    pinMode(cfg::PIN_DBG_AUX,  OUTPUT); digitalWrite(cfg::PIN_DBG_AUX,  LOW);

    pinMode(cfg::PIN_RS485_DE, OUTPUT);
    digitalWrite(cfg::PIN_RS485_DE, LOW);   // receive by default
    RS485.setTX(cfg::PIN_RS485_TX);
    RS485.setRX(cfg::PIN_RS485_RX);
    RS485.setFIFOSize(512);
    RS485.begin(cfg::RS485_BAUD);

    reel_hw::begin();
    reel_hw::set_input_change_cb(on_input_change);
    reel_hw::set_module_count_cb(on_module_count);

    s_decoder = decoder_create(on_frame, nullptr);
    s_boot_ms = millis();

    Serial.println("\n[boot] SmartReel Core PRODUCTION firmware");
    Serial.printf("[boot] v%u.%u.%u built %s (%s)\n",
                  g_fw_tag.major, g_fw_tag.minor, g_fw_tag.patch,
                  g_fw_tag.build, CORE_BUILD_TAG);

    // Mount LittleFS now so the FW_BEGIN handler's Update.begin() just
    // opens a file -- the one-time format erase mustn't happen
    // mid-transaction or it blows the master's FW_BEGIN timeout.
    if (!LittleFS.begin()) {
        Serial.println("[boot] LittleFS mount failed -- formatting");
        LittleFS.format();
        LittleFS.begin();
    }
    Serial.printf("[boot] RS485 UART0 TX=%u RX=%u DE=%u @ %lu baud, addr=0x%02X\n",
                  cfg::PIN_RS485_TX, cfg::PIN_RS485_RX, cfg::PIN_RS485_DE,
                  (unsigned long)cfg::RS485_BAUD, ADDR_CORE);
    Serial.printf("[boot] ADS1115 %s; %u ports, up to %u modules each\n",
                  reel_hw::ads_ok() ? "found" : "NOT FOUND",
                  cfg::N_PORTS, cfg::MAX_MODULES_PER_PORT);
    print_help();
}

void loop() {
    // 1. Service the RS485 link first -- replies sent inline from on_frame().
    while (RS485.available()) {
        s_rx_bytes++;
        decoder_feed(s_decoder, (uint8_t)RS485.read());
    }
    poll_console();

    // 2. Hardware sampling -- suspended during a firmware update (flash
    //    writes disable XIP/interrupts; the bus is exclusive anyway).
    if (!s_fw_active) {
        uint32_t now = millis();
        static uint32_t last_in = 0, last_sense = 0;
        if (now - last_in >= cfg::INPUT_SAMPLE_MS)   { last_in = now;    reel_hw::sample_inputs(); }
        if (now - last_sense >= cfg::SENSE_SAMPLE_MS) { last_sense = now; reel_hw::sample_sense();  }
    }

    // 3. Debug LEDs.
    update_debug_leds();
}
