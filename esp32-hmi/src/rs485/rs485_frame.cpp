#include "rs485/rs485_frame.h"
#include "rs485/crc16.h"

#include <string.h>
#include <stdlib.h>

namespace rs485 {

// =====================================================================
//  Encoder
// =====================================================================
size_t encode_frame(uint8_t* out, size_t out_cap,
                    uint8_t addr, uint8_t seq, uint8_t type,
                    const uint8_t* payload, size_t payload_len) {
    if (!out)                          return 0;
    if (payload_len > MAX_PAYLOAD)     return 0;

    // LEN counts ADDR + SEQ + TYPE + payload.
    const size_t len_field = 3 + payload_len;
    const size_t total     = 2 /*sync*/ + 2 /*len*/ + len_field + 2 /*crc*/;
    if (total > out_cap)               return 0;

    uint8_t* p = out;
    *p++ = SYNC0;
    *p++ = SYNC1;
    *p++ = (uint8_t)(len_field >> 8);
    *p++ = (uint8_t)(len_field & 0xFF);

    // CRC is computed over ADDR..end-of-PAYLOAD. Track that span.
    uint8_t* crc_start = p;

    *p++ = addr;
    *p++ = seq;
    *p++ = type;
    if (payload_len) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }

    uint16_t crc = crc16_ccitt_false(crc_start, len_field);
    *p++ = (uint8_t)(crc >> 8);
    *p++ = (uint8_t)(crc & 0xFF);

    return (size_t)(p - out);
}

// =====================================================================
//  Streaming decoder
//
//  The state machine hunts for SYNC0 SYNC1, then reads LEN_H/LEN_L,
//  then collects LEN bytes into `payload`, then CRC_H/CRC_L. Any byte
//  that violates the state machine's expectation (LEN > MAX_PAYLOAD+3,
//  CRC mismatch, etc.) drops us back to HUNT and the next 0xAA 0x55
//  starts a new frame.
// =====================================================================
enum class DState : uint8_t {
    HUNT_SYNC0,
    HUNT_SYNC1,
    READ_LEN_H,
    READ_LEN_L,
    READ_BODY,    // ADDR + SEQ + TYPE + payload (all stored together)
    READ_CRC_H,
    READ_CRC_L,
};

struct Decoder {
    DState        state;
    uint16_t      expected_len;     // body length (ADDR + SEQ + TYPE + payload)
    uint16_t      body_filled;
    uint8_t       body[MAX_FRAME_BYTES];  // worst case fits whole frame body
    uint16_t      rx_crc;            // CRC field from the wire
    FrameCallback cb;
    void*         user;
};

Decoder* decoder_create(FrameCallback cb, void* user) {
    Decoder* d = (Decoder*)calloc(1, sizeof(Decoder));
    if (!d) return nullptr;
    d->cb    = cb;
    d->user  = user;
    d->state = DState::HUNT_SYNC0;
    return d;
}

void decoder_destroy(Decoder* d) { free(d); }

void decoder_reset(Decoder* d) {
    if (!d) return;
    d->state        = DState::HUNT_SYNC0;
    d->expected_len = 0;
    d->body_filled  = 0;
    d->rx_crc       = 0;
}

void decoder_feed(Decoder* d, uint8_t b) {
    if (!d) return;
    switch (d->state) {
        case DState::HUNT_SYNC0:
            if (b == SYNC0) d->state = DState::HUNT_SYNC1;
            break;

        case DState::HUNT_SYNC1:
            // The doc specifies the sync pair as 0xAA 0x55 -- if the
            // second byte is a stray 0xAA we stay one step in (it could
            // be the start of a new frame); otherwise resync.
            if (b == SYNC1)      d->state = DState::READ_LEN_H;
            else if (b == SYNC0) /* stay */;
            else                 d->state = DState::HUNT_SYNC0;
            break;

        case DState::READ_LEN_H:
            d->expected_len = (uint16_t)b << 8;
            d->state = DState::READ_LEN_L;
            break;

        case DState::READ_LEN_L:
            d->expected_len |= b;
            // Sanity check: body must include at least ADDR+SEQ+TYPE.
            if (d->expected_len < 3 ||
                d->expected_len > (3 + MAX_PAYLOAD)) {
                decoder_reset(d);
                break;
            }
            d->body_filled = 0;
            d->state = DState::READ_BODY;
            break;

        case DState::READ_BODY:
            d->body[d->body_filled++] = b;
            if (d->body_filled == d->expected_len) {
                d->state = DState::READ_CRC_H;
            }
            break;

        case DState::READ_CRC_H:
            d->rx_crc = (uint16_t)b << 8;
            d->state = DState::READ_CRC_L;
            break;

        case DState::READ_CRC_L: {
            d->rx_crc |= b;
            uint16_t calc = crc16_ccitt_false(d->body, d->expected_len);
            if (calc == d->rx_crc && d->cb) {
                const uint8_t addr = d->body[0];
                const uint8_t seq  = d->body[1];
                const uint8_t type = d->body[2];
                const size_t  plen = d->expected_len - 3;
                d->cb(addr, seq, type, plen ? &d->body[3] : nullptr, plen, d->user);
            }
            // Whether CRC matched or not we go back to hunting; bad
            // CRCs just get dropped silently.
            decoder_reset(d);
            break;
        }
    }
}

} // namespace rs485
