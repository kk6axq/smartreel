// =====================================================================
//  SmartReel Core -- RS485 slave test firmware (RP2040)
//
//  Bench rig for validating the ESP32 HMI's RS485 master + UI plumbing
//  with no reel hardware attached. We speak the slave half of
//  docs/smartreel-rs485-protocol.md and let a human inject fake reel
//  events from the USB serial console.
//
//  Two serial ports are in play:
//    Serial   -- USB CDC. The operator console (type 'help').
//    Serial1  -- UART0 on GPIO16/17, the SP3485 RS485 link to the HMI.
//                GPIO18 is the manual DE/RE# driver-enable line.
//
//  Loop structure: drain the RS485 UART into the streaming frame
//  decoder (which fires on_frame() for each valid request and we reply
//  inline), then drain the USB console. No blocking calls, so request
//  latency stays well under the HMI's ~50 ms transaction timeout.
// =====================================================================

#include <Arduino.h>
#include <Updater.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>

#include "rs485/rs485_proto.h"
#include "rs485/rs485_frame.h"
#include "sha256.h"

using namespace rs485;

// Bump this to make an OTA target visually distinct from what's running.
#define CORE_BUILD_TAG "dev"

// ---- Pin map (see platformio.ini header) ---------------------------
static constexpr uint8_t PIN_RS485_TX = 16;   // -> SP3485 DI
static constexpr uint8_t PIN_RS485_RX = 17;   // <- SP3485 RO
static constexpr uint8_t PIN_RS485_DE = 18;   // -> SP3485 DE + RE#
// 115200 to match the HMI: its auto-direction RS485 can't switch fast
// enough for 921600 (see esp32-hmi board_pins.h / RS485 design notes).
static constexpr uint32_t RS485_BAUD  = 115200;

#define RS485 Serial1

// ---- Fake "device" state -------------------------------------------
static constexpr int N_REELS = 4;

static uint32_t s_boot_ms = 0;

struct ReelState {
    bool     present;
    uint16_t sense_mv;
    uint16_t inputs;        // 16 input lines (two chained 74HC165s)
    uint8_t  id[4];
};
static ReelState s_reel[N_REELS];

static uint16_t s_error_flags = 0;

static uint8_t reels_present_bitmap() {
    uint8_t b = 0;
    for (int i = 0; i < N_REELS; ++i) if (s_reel[i].present) b |= (1u << i);
    return b;
}

// ---- Async event ring ----------------------------------------------
// Events live here until the HMI ACKs their SEQ (MSG_ACK_EVENTS). Until
// then every POLL re-reports them, which is exactly the resend
// behaviour the protocol doc calls for.
struct PendingEvt {
    bool    used;
    uint8_t seq;
    uint8_t type;
    uint8_t len;
    uint8_t data[64];
};
static constexpr int EVT_RING = 16;
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
            Serial.printf("[evt] queued type=0x%02X seq=%u len=%u\n",
                          type, s_evt[i].seq, len);
            return true;
        }
    }
    Serial.println("[evt] ring full -- event dropped (HMI not ACKing?)");
    return false;
}

static void ack_event_seq(uint8_t seq) {
    for (int i = 0; i < EVT_RING; ++i) {
        if (s_evt[i].used && s_evt[i].seq == seq) {
            s_evt[i].used = false;
            return;
        }
    }
}

// ---- Diagnostics ---------------------------------------------------
static uint32_t s_rx_frames = 0, s_tx_frames = 0, s_polls = 0;
static uint32_t s_rx_bytes  = 0;   // raw bytes off the RS485 UART

// =====================================================================
//  RS485 transmit (half-duplex turnaround)
// =====================================================================
static uint8_t s_txbuf[MAX_FRAME_BYTES];

static void rs485_send(uint8_t type, uint8_t seq,
                       const uint8_t* payload, size_t plen) {
    size_t n = encode_frame(s_txbuf, sizeof(s_txbuf),
                            ADDR_MASTER, seq, type, payload, plen);
    if (!n) return;

    // Turnaround delay: the HMI is a half-duplex master with an
    // auto-direction transceiver. After it finishes sending a request
    // it needs a moment to switch from drive back to receive. If we
    // reply too soon it misses the start of our frame (shows up as a
    // master-side timeout with no CRC error). Wait before driving.
    delayMicroseconds(500);

    digitalWrite(PIN_RS485_DE, HIGH);   // assert driver
    RS485.write(s_txbuf, n);
    RS485.flush();                      // NOTE: Pico SDK flush only drains
                                        // the TX FIFO, not the shift reg --
                                        // the last byte is still clocking out.
    // Hold DE until the final byte fully shifts out: one byte = 10 bits /
    // 115200 = ~87us. Dropping too early truncates the CRC and the master
    // sees a timeout (not a CRC error). 200us gives comfortable margin.
    delayMicroseconds(200);
    digitalWrite(PIN_RS485_DE, LOW);    // back to receive
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
    // POLL response payload: count, then {type, seq, len, data} per event.
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
    // major, minor, patch, build_id(4 BE), hw_rev
    const uint8_t v[8] = { 0, 1, 0, 0xCA, 0xFE, 0xBA, 0xBE, 0x01 };
    rs485_send(mk_response(MSG_GET_VERSION), seq, v, sizeof(v));
}

static void handle_get_status(uint8_t seq) {
    uint32_t up = (millis() - s_boot_ms) / 1000;
    uint8_t v[7];
    v[0] = (up >> 24) & 0xFF; v[1] = (up >> 16) & 0xFF;
    v[2] = (up >>  8) & 0xFF; v[3] =  up        & 0xFF;
    v[4] = reels_present_bitmap();
    v[5] = (s_error_flags >> 8) & 0xFF;
    v[6] =  s_error_flags       & 0xFF;
    rs485_send(mk_response(MSG_GET_STATUS), seq, v, sizeof(v));
}

static void append_reel_record(uint8_t* buf, size_t& off, int r) {
    buf[off++] = (uint8_t)r;
    buf[off++] = s_reel[r].present ? 1 : 0;
    buf[off++] = (s_reel[r].sense_mv >> 8) & 0xFF;
    buf[off++] =  s_reel[r].sense_mv       & 0xFF;
    buf[off++] = sizeof(s_reel[r].id);
    memcpy(&buf[off], s_reel[r].id, sizeof(s_reel[r].id));
    off += sizeof(s_reel[r].id);
}

static void handle_get_reel_info(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 1) { send_error(MSG_GET_REEL_INFO, seq, ERR_BAD_PAYLOAD); return; }
    uint8_t reel_id = p[0];
    uint8_t buf[MAX_PAYLOAD];
    size_t  off = 1;
    uint8_t count = 0;
    if (reel_id == REEL_ID_ALL) {
        for (int r = 0; r < N_REELS; ++r) { append_reel_record(buf, off, r); count++; }
    } else if (reel_id < N_REELS) {
        append_reel_record(buf, off, reel_id); count = 1;
    } else {
        send_error(MSG_GET_REEL_INFO, seq, ERR_REEL_ABSENT); return;
    }
    buf[0] = count;
    rs485_send(mk_response(MSG_GET_REEL_INFO), seq, buf, off);
}

static void handle_read_inputs(uint8_t seq, const uint8_t* p, size_t len) {
    if (len < 1) { send_error(MSG_READ_INPUTS, seq, ERR_BAD_PAYLOAD); return; }
    uint8_t reel_id = p[0];
    if (reel_id >= N_REELS) { send_error(MSG_READ_INPUTS, seq, ERR_REEL_ABSENT); return; }
    uint8_t v[2] = { (uint8_t)(s_reel[reel_id].inputs >> 8),
                     (uint8_t)(s_reel[reel_id].inputs & 0xFF) };
    rs485_send(mk_response(MSG_READ_INPUTS), seq, v, sizeof(v));
}

// =====================================================================
//  Firmware update receiver (FW_* messages, arduino-pico OTA staging)
//
//  Flow per docs/smartreel-rs485-protocol.md:
//    FW_BEGIN  -> Update.begin() (opens LittleFS staging), reply OK/READY
//    FW_CHUNK  -> Update.write() the data at the expected offset, hash
//                 it, reply with the byte count received so far
//    FW_VERIFY -> compare our SHA-256 to the one announced in FW_BEGIN
//    FW_COMMIT -> Update.end() (stages PicoOTA), reply OK, then reboot;
//                 the core's OTA bootloader flashes the image on boot.
// =====================================================================
static bool       s_fw_active   = false;
static uint32_t   s_fw_total    = 0;
static uint32_t   s_fw_received = 0;
static sha256_ctx s_fw_sha;
static uint8_t    s_fw_expected[32];

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void handle_fw_begin(uint8_t seq, const uint8_t* p, size_t len) {
    // payload: total_size(4) + sha256(32) + version(4)
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
    send_ack(MSG_FW_BEGIN, seq);    // ready (LittleFS staging, no slow erase)
}

static void handle_fw_chunk(uint8_t seq, const uint8_t* p, size_t len) {
    if (!s_fw_active || len < 4) { send_error(MSG_FW_CHUNK, seq, ERR_FW_STATE); return; }
    uint32_t offset = be32(p);
    const uint8_t* data = p + 4;
    size_t dlen = len - 4;

    if (offset == s_fw_received) {
        // next expected chunk: write + hash it
        if (Update.write((uint8_t*)data, dlen) != dlen) {
            Serial.println("[fw] write failed");
            send_error(MSG_FW_CHUNK, seq, ERR_BUSY);
            return;
        }
        sha256_update(&s_fw_sha, data, dlen);
        s_fw_received += dlen;
    } else if (offset + dlen <= s_fw_received) {
        // already have it (a retransmit) -- just re-ACK
    } else {
        // gap: master must resend from where we are
        send_error(MSG_FW_CHUNK, seq, ERR_FW_OOR);
        return;
    }
    // ACK with bytes-received-so-far so the master advances
    uint8_t ack[4]; put_be32(ack, s_fw_received);
    rs485_send(mk_response(MSG_FW_CHUNK), seq, ack, sizeof(ack));
}

static void handle_fw_verify(uint8_t seq) {
    if (!s_fw_active) { send_error(MSG_FW_VERIFY, seq, ERR_FW_STATE); return; }
    uint8_t got[32];
    sha256_ctx tmp = s_fw_sha;          // copy so we don't disturb state
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

static void on_frame(uint8_t addr, uint8_t seq, uint8_t type,
                     const uint8_t* payload, size_t payload_len,
                     void* /*user*/) {
    // Only act on frames for us. (Responses carry the master's address
    // and shouldn't appear on this half of the link anyway.)
    if (addr != ADDR_CORE) return;
    if (is_response(type))  return;
    s_rx_frames++;

    switch (type) {
        case MSG_PING:          send_ack(MSG_PING, seq);                 break;
        case MSG_GET_VERSION:   handle_get_version(seq);                 break;
        case MSG_GET_STATUS:    handle_get_status(seq);                  break;
        case MSG_POLL:          handle_poll(seq);                        break;
        case MSG_ACK_EVENTS:    handle_ack_events(seq, payload, payload_len); break;
        case MSG_RESET:
            Serial.printf("[rs485] RESET reason=%u (test rig: not resetting)\n",
                          payload_len ? payload[0] : 0);
            send_ack(MSG_RESET, seq);
            break;
        case MSG_GET_REEL_INFO: handle_get_reel_info(seq, payload, payload_len); break;
        case MSG_READ_INPUTS:   handle_read_inputs(seq, payload, payload_len);   break;
        case MSG_SET_POLL_RATE: send_ack(MSG_SET_POLL_RATE, seq);        break;
        // LED commands: accept and ACK so the HMI's LED paths exercise
        // cleanly; nothing is driven on this rig.
        case MSG_SET_REEL_PIXELS: send_ack(MSG_SET_REEL_PIXELS, seq);    break;
        case MSG_FILL_REEL:       send_ack(MSG_FILL_REEL, seq);          break;
        case MSG_SET_BRIGHTNESS:  send_ack(MSG_SET_BRIGHTNESS, seq);     break;
        case MSG_SET_ANIMATION:   send_ack(MSG_SET_ANIMATION, seq);      break;
        case MSG_COMMIT:          send_ack(MSG_COMMIT, seq);             break;
        // Firmware update (arduino-pico OTA staging)
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
//  USB console -- fake-event injection
// =====================================================================
static void print_help() {
    Serial.println(F(
        "\nSmartReel Core test rig -- console commands:\n"
        "  help                         this text\n"
        "  status                       dump local reel/event state\n"
        "  stats                        RS485 frame counters\n"
        "  reels <hex>                  set present bitmap (e.g. 'reels 0x5')\n"
        "  insert <reel> [mv]           reel inserted (default 1650 mV) + event\n"
        "  remove <reel>                reel removed + event\n"
        "  press  <reel> <bit>          toggle one input bit (0-15) + event\n"
        "  input  <reel> <newhex> [prevhex]  set 16-bit input mask + event\n"
        "  sense  <reel> <mv> <thr>     SENSE_THRESHOLD event\n"
        "  log    <level> <message...>  LOG event to the HMI\n"
        "Reels are 0-3. Numbers accept 0x.. hex or decimal.\n"));
}

static void emit_input_change(int reel, uint16_t prev, uint16_t now) {
    uint32_t ts = millis();
    uint8_t d[9] = {
        (uint8_t)reel,
        (uint8_t)(prev >> 8), (uint8_t)(prev & 0xFF),
        (uint8_t)(now  >> 8), (uint8_t)(now  & 0xFF),
        (uint8_t)(ts >> 24), (uint8_t)(ts >> 16),
        (uint8_t)(ts >>  8), (uint8_t)(ts & 0xFF),
    };
    enqueue_event(EVT_INPUT_CHANGE, d, sizeof(d));
}

static void print_status() {
    Serial.printf("present bitmap=0x%02X  errflags=0x%04X  uptime=%lus\n",
                  reels_present_bitmap(), s_error_flags,
                  (unsigned long)((millis() - s_boot_ms) / 1000));
    for (int r = 0; r < N_REELS; ++r) {
        Serial.printf("  reel %d: present=%d sense=%u mV inputs=0x%04X id=%02X%02X%02X%02X\n",
                      r, s_reel[r].present, s_reel[r].sense_mv, s_reel[r].inputs,
                      s_reel[r].id[0], s_reel[r].id[1], s_reel[r].id[2], s_reel[r].id[3]);
    }
    int pending = 0;
    for (int i = 0; i < EVT_RING; ++i) if (s_evt[i].used) pending++;
    Serial.printf("  pending (un-ACKed) events: %d\n", pending);
}

// strtol with base 0 (auto-detect 0x); returns def on empty token.
static long tok_num(const char* t, long def) {
    if (!t || !*t) return def;
    return strtol(t, nullptr, 0);
}

static void process_console_line(char* line) {
    char* cmd = strtok(line, " \t");
    if (!cmd) return;

    if (!strcmp(cmd, "help")) { print_help(); return; }
    if (!strcmp(cmd, "ver"))  { Serial.println("build " CORE_BUILD_TAG " " __DATE__ " " __TIME__); return; }
    if (!strcmp(cmd, "status")) { print_status(); return; }
    if (!strcmp(cmd, "blast")) {
        // Actively drive the bus (DE high) with a 0x55 stream for <ms>
        // ms -- a reverse-direction link test. 0x55 = alternating bits,
        // so a meter on A-B reads a mid-level and the ESP32's raw
        // 'listen' should count bytes if the bus + its RX path work.
        long ms = tok_num(strtok(nullptr, " \t"), 300);
        digitalWrite(PIN_RS485_DE, HIGH);
        delayMicroseconds(10);
        uint32_t deadline = millis() + ms, n = 0;
        while ((long)(millis() - deadline) < 0) { RS485.write((uint8_t)0x55); n++; }
        RS485.flush(); delayMicroseconds(40);
        digitalWrite(PIN_RS485_DE, LOW);
        Serial.printf("blasted %lu bytes (0x55) over %ld ms\n", (unsigned long)n, ms);
        return;
    }
    if (!strcmp(cmd, "stats")) {
        Serial.printf("rx_bytes=%lu rx_frames=%lu tx_frames=%lu polls=%lu\n",
                      (unsigned long)s_rx_bytes, (unsigned long)s_rx_frames,
                      (unsigned long)s_tx_frames, (unsigned long)s_polls);
        return;
    }
    if (!strcmp(cmd, "reels")) {
        long b = tok_num(strtok(nullptr, " \t"), -1);
        if (b < 0) { Serial.println("usage: reels <hex>"); return; }
        for (int r = 0; r < N_REELS; ++r) s_reel[r].present = (b >> r) & 1;
        Serial.printf("present bitmap -> 0x%02X\n", reels_present_bitmap());
        return;
    }
    if (!strcmp(cmd, "insert")) {
        int reel = (int)tok_num(strtok(nullptr, " \t"), -1);
        if (reel < 0 || reel >= N_REELS) { Serial.println("usage: insert <reel 0-3> [mv]"); return; }
        long mv = tok_num(strtok(nullptr, " \t"), 1650);
        s_reel[reel].present  = true;
        s_reel[reel].sense_mv = (uint16_t)mv;
        uint8_t d[7] = { (uint8_t)reel,
                         (uint8_t)(s_reel[reel].sense_mv >> 8),
                         (uint8_t)(s_reel[reel].sense_mv & 0xFF),
                         s_reel[reel].id[0], s_reel[reel].id[1],
                         s_reel[reel].id[2], s_reel[reel].id[3] };
        enqueue_event(EVT_REEL_INSERTED, d, sizeof(d));
        return;
    }
    if (!strcmp(cmd, "remove")) {
        int reel = (int)tok_num(strtok(nullptr, " \t"), -1);
        if (reel < 0 || reel >= N_REELS) { Serial.println("usage: remove <reel 0-3>"); return; }
        s_reel[reel].present = false;
        uint8_t d[1] = { (uint8_t)reel };
        enqueue_event(EVT_REEL_REMOVED, d, sizeof(d));
        return;
    }
    if (!strcmp(cmd, "press")) {
        int reel = (int)tok_num(strtok(nullptr, " \t"), -1);
        int bit  = (int)tok_num(strtok(nullptr, " \t"), -1);
        if (reel < 0 || reel >= N_REELS || bit < 0 || bit > 15) {
            Serial.println("usage: press <reel 0-3> <bit 0-15>"); return;
        }
        uint16_t prev = s_reel[reel].inputs;
        s_reel[reel].inputs ^= (1u << bit);
        emit_input_change(reel, prev, s_reel[reel].inputs);
        return;
    }
    if (!strcmp(cmd, "input")) {
        int reel = (int)tok_num(strtok(nullptr, " \t"), -1);
        char* nt = strtok(nullptr, " \t");
        if (reel < 0 || reel >= N_REELS || !nt) {
            Serial.println("usage: input <reel 0-3> <newhex> [prevhex]"); return;
        }
        uint16_t now  = (uint16_t)strtol(nt, nullptr, 16);
        char* pt = strtok(nullptr, " \t");
        uint16_t prev = pt ? (uint16_t)strtol(pt, nullptr, 16) : s_reel[reel].inputs;
        s_reel[reel].inputs = now;
        emit_input_change(reel, prev, now);
        return;
    }
    if (!strcmp(cmd, "sense")) {
        int reel = (int)tok_num(strtok(nullptr, " \t"), -1);
        long mv  = tok_num(strtok(nullptr, " \t"), -1);
        long thr = tok_num(strtok(nullptr, " \t"), -1);
        if (reel < 0 || reel >= N_REELS || mv < 0 || thr < 0) {
            Serial.println("usage: sense <reel 0-3> <mv> <threshold_id>"); return;
        }
        uint8_t d[4] = { (uint8_t)reel, (uint8_t)((mv >> 8) & 0xFF),
                         (uint8_t)(mv & 0xFF), (uint8_t)thr };
        enqueue_event(EVT_SENSE_THRESHOLD, d, sizeof(d));
        return;
    }
    if (!strcmp(cmd, "log")) {
        long level = tok_num(strtok(nullptr, " \t"), 0);
        char* msg  = strtok(nullptr, "");   // rest of line
        if (!msg) msg = (char*)"";
        while (*msg == ' ') msg++;
        uint8_t d[64];
        d[0] = (uint8_t)level;
        size_t n = strlen(msg);
        if (n > sizeof(d) - 1) n = sizeof(d) - 1;
        memcpy(&d[1], msg, n);
        enqueue_event(EVT_LOG, d, (uint8_t)(1 + n));
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
//  Arduino entry points
// =====================================================================
void setup() {
    Serial.begin(115200);

    pinMode(PIN_RS485_DE, OUTPUT);
    digitalWrite(PIN_RS485_DE, LOW);   // receive by default

    RS485.setTX(PIN_RS485_TX);
    RS485.setRX(PIN_RS485_RX);
    RS485.setFIFOSize(512);
    RS485.begin(RS485_BAUD);

    for (int r = 0; r < N_REELS; ++r) {
        s_reel[r].present  = false;
        s_reel[r].sense_mv = 1650;
        s_reel[r].inputs   = 0;
        s_reel[r].id[0] = 0xDE; s_reel[r].id[1] = 0xAD;
        s_reel[r].id[2] = (uint8_t)r; s_reel[r].id[3] = 0x01;
    }

    s_decoder = decoder_create(on_frame, nullptr);
    s_boot_ms = millis();

    Serial.println("\n[boot] SmartReel Core RS485 test rig");
    Serial.println("[boot] build " CORE_BUILD_TAG " " __DATE__ " " __TIME__);

    // Mount (and on first boot, format) LittleFS now so the FW_BEGIN
    // handler's Update.begin() just opens a file -- the one-time format
    // erase mustn't happen mid-transaction or it blows the master's
    // FW_BEGIN timeout.
    if (!LittleFS.begin()) {
        Serial.println("[boot] LittleFS mount failed -- formatting");
        LittleFS.format();
        LittleFS.begin();
    }
    Serial.println("[boot] LittleFS ready (OTA staging)");
    Serial.printf("[boot] RS485 UART0 TX=%u RX=%u DE=%u @ %lu baud, addr=0x%02X\n",
                  PIN_RS485_TX, PIN_RS485_RX, PIN_RS485_DE,
                  (unsigned long)RS485_BAUD, ADDR_CORE);
    print_help();
}

void loop() {
    // Service the RS485 link first -- replies are sent inline from
    // on_frame() as soon as a full request decodes.
    while (RS485.available()) {
        s_rx_bytes++;
        decoder_feed(s_decoder, (uint8_t)RS485.read());
    }
    poll_console();

    // Link heartbeat: every 2 s, report raw RX activity so a dead wire
    // (rx_bytes stuck at 0) is obvious vs. bytes-but-no-frames.
    static uint32_t last_hb = 0;
    static uint32_t last_bytes = 0, last_frames = 0;
    if (millis() - last_hb >= 2000) {
        last_hb = millis();
        if (s_rx_bytes != last_bytes || s_rx_frames != last_frames) {
            Serial.printf("[hb] rx_bytes=%lu rx_frames=%lu polls=%lu\n",
                          (unsigned long)s_rx_bytes, (unsigned long)s_rx_frames,
                          (unsigned long)s_polls);
            last_bytes = s_rx_bytes; last_frames = s_rx_frames;
        }
    }
}
