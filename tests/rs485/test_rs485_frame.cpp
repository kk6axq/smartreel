// =====================================================================
//  Host unit tests for the shared RS485 frame codec + CRC.
//
//  This is the wire format spoken by BOTH firmwares (esp32-hmi master and
//  core-fw slave), so a regression here breaks the whole bus. The codec is
//  plain C++ with no hardware dependencies, so we compile and run it on the
//  host. See tests/README.md to build/run.
//
//  Covers: CRC-16/CCITT-FALSE known-answer, encode bounds, encode->decode
//  round-trips across payload sizes, streaming resync after garbage, and
//  rejection of corrupt CRC / bad length frames.
// =====================================================================
#include "rs485/rs485_frame.h"
#include "rs485/rs485_proto.h"
#include "rs485/crc16.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                  \
    } while (0)

// --- Capture the last frame the decoder hands back -------------------
struct Captured {
    int      count = 0;
    uint8_t  addr  = 0;
    uint8_t  seq   = 0;
    uint8_t  type  = 0;
    std::vector<uint8_t> payload;
};

static void on_frame(uint8_t addr, uint8_t seq, uint8_t type,
                     const uint8_t* payload, size_t plen, void* user) {
    auto* c = static_cast<Captured*>(user);
    c->count++;
    c->addr = addr;
    c->seq  = seq;
    c->type = type;
    c->payload.assign(payload, payload + plen);
}

// --- Tests -----------------------------------------------------------

static void test_crc_known_answer() {
    std::printf("test_crc_known_answer\n");
    // CRC-16/CCITT-FALSE of "123456789" is the canonical 0x29B1.
    const uint8_t v[] = "123456789";
    CHECK(crc16_ccitt_false(v, 9) == 0x29B1);
    // Empty input returns the init value, unchanged.
    CHECK(crc16_ccitt_false(nullptr, 0) == 0xFFFF);
}

static void test_encode_bounds() {
    std::printf("test_encode_bounds\n");
    uint8_t out[rs485::MAX_FRAME_BYTES];
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    // null out buffer rejected
    CHECK(rs485::encode_frame(nullptr, sizeof(out), rs485::ADDR_CORE, 0,
                              rs485::MSG_PING, payload, 8) == 0);
    // payload over MAX_PAYLOAD rejected
    CHECK(rs485::encode_frame(out, sizeof(out), rs485::ADDR_CORE, 0,
                              rs485::MSG_PING, payload, rs485::MAX_PAYLOAD + 1) == 0);
    // out_cap too small rejected (need 2+2+3+8+2 = 17 bytes; give 16)
    CHECK(rs485::encode_frame(out, 16, rs485::ADDR_CORE, 0,
                              rs485::MSG_PING, payload, 8) == 0);
    // exact fit succeeds
    CHECK(rs485::encode_frame(out, 17, rs485::ADDR_CORE, 0,
                              rs485::MSG_PING, payload, 8) == 17);
}

static void test_encoded_layout() {
    std::printf("test_encoded_layout\n");
    uint8_t out[rs485::MAX_FRAME_BYTES];
    uint8_t payload[2] = {0xDE, 0xAD};
    size_t n = rs485::encode_frame(out, sizeof(out), rs485::ADDR_MASTER,
                                   0x42, rs485::MSG_GET_VERSION, payload, 2);
    CHECK(n == 11);                 // 2 sync + 2 len + (3 + 2) body + 2 crc
    CHECK(out[0] == rs485::SYNC0);
    CHECK(out[1] == rs485::SYNC1);
    CHECK(out[2] == 0x00);          // LEN high (body len = 5)
    CHECK(out[3] == 0x05);          // LEN low
    CHECK(out[4] == rs485::ADDR_MASTER);
    CHECK(out[5] == 0x42);
    CHECK(out[6] == rs485::MSG_GET_VERSION);
    CHECK(out[7] == 0xDE);
    CHECK(out[8] == 0xAD);
    // CRC is over body[ADDR..payload] = out[4..8].
    uint16_t crc = crc16_ccitt_false(&out[4], 5);
    CHECK(out[9]  == (uint8_t)(crc >> 8));
    CHECK(out[10] == (uint8_t)(crc & 0xFF));
}

// Encode a frame, stream its bytes through the decoder, and confirm the
// fields round-trip. Tries 0, 1, a middling, and MAX_PAYLOAD lengths.
static void test_roundtrip() {
    std::printf("test_roundtrip\n");
    const size_t sizes[] = {0, 1, 7, 64, rs485::MAX_PAYLOAD};
    for (size_t plen : sizes) {
        std::vector<uint8_t> payload(plen);
        for (size_t i = 0; i < plen; ++i) payload[i] = (uint8_t)(i * 7 + 1);

        uint8_t out[rs485::MAX_FRAME_BYTES];
        size_t n = rs485::encode_frame(out, sizeof(out), rs485::ADDR_CORE,
                                       (uint8_t)(plen & 0xFF), rs485::MSG_READ_INPUTS,
                                       plen ? payload.data() : nullptr, plen);
        CHECK(n > 0);

        Captured cap;
        rs485::Decoder* d = rs485::decoder_create(on_frame, &cap);
        for (size_t i = 0; i < n; ++i) rs485::decoder_feed(d, out[i]);

        CHECK(cap.count == 1);
        CHECK(cap.addr == rs485::ADDR_CORE);
        CHECK(cap.seq == (uint8_t)(plen & 0xFF));
        CHECK(cap.type == rs485::MSG_READ_INPUTS);
        CHECK(cap.payload.size() == plen);
        CHECK(memcmp(cap.payload.data(), payload.data(), plen) == 0);
        rs485::decoder_destroy(d);
    }
}

// Leading garbage (including a stray sync byte) must not stop a valid frame
// that follows from being decoded.
static void test_resync_after_garbage() {
    std::printf("test_resync_after_garbage\n");
    uint8_t payload[3] = {0x11, 0x22, 0x33};
    uint8_t frame[rs485::MAX_FRAME_BYTES];
    size_t n = rs485::encode_frame(frame, sizeof(frame), rs485::ADDR_CORE, 9,
                                   rs485::MSG_POLL, payload, 3);

    Captured cap;
    rs485::Decoder* d = rs485::decoder_create(on_frame, &cap);
    // Junk, a lone SYNC0, more junk, then the real frame.
    const uint8_t junk[] = {0x00, 0xFF, rs485::SYNC0, 0x12, 0x34, 0xAA, 0xAA};
    for (uint8_t b : junk) rs485::decoder_feed(d, b);
    for (size_t i = 0; i < n; ++i) rs485::decoder_feed(d, frame[i]);

    CHECK(cap.count == 1);
    CHECK(cap.type == rs485::MSG_POLL);
    CHECK(cap.payload.size() == 3);
    rs485::decoder_destroy(d);
}

// A single corrupted payload byte must be caught by the CRC and dropped.
static void test_crc_rejection() {
    std::printf("test_crc_rejection\n");
    uint8_t payload[4] = {0xAB, 0xCD, 0xEF, 0x01};
    uint8_t frame[rs485::MAX_FRAME_BYTES];
    size_t n = rs485::encode_frame(frame, sizeof(frame), rs485::ADDR_CORE, 1,
                                   rs485::MSG_GET_STATUS, payload, 4);
    frame[6] ^= 0xFF;   // flip a payload byte after encoding

    Captured cap;
    rs485::Decoder* d = rs485::decoder_create(on_frame, &cap);
    for (size_t i = 0; i < n; ++i) rs485::decoder_feed(d, frame[i]);
    CHECK(cap.count == 0);   // corrupt frame must not surface

    // And the decoder must recover for the very next (clean) frame.
    for (size_t i = 0; i < n; ++i) rs485::decoder_feed(d, frame[i]);   // still corrupt
    frame[6] ^= 0xFF;        // repair
    for (size_t i = 0; i < n; ++i) rs485::decoder_feed(d, frame[i]);
    CHECK(cap.count == 1);
    rs485::decoder_destroy(d);
}

// An out-of-range LEN field must reset the decoder, not overflow its buffer.
static void test_bad_length_rejected() {
    std::printf("test_bad_length_rejected\n");
    Captured cap;
    rs485::Decoder* d = rs485::decoder_create(on_frame, &cap);
    // sync, then LEN = 2 (below the 3-byte ADDR+SEQ+TYPE minimum).
    rs485::decoder_feed(d, rs485::SYNC0);
    rs485::decoder_feed(d, rs485::SYNC1);
    rs485::decoder_feed(d, 0x00);
    rs485::decoder_feed(d, 0x02);
    // sync, then LEN = MAX_PAYLOAD + 4 (above the cap).
    uint16_t too_big = 3 + rs485::MAX_PAYLOAD + 1;
    rs485::decoder_feed(d, rs485::SYNC0);
    rs485::decoder_feed(d, rs485::SYNC1);
    rs485::decoder_feed(d, (uint8_t)(too_big >> 8));
    rs485::decoder_feed(d, (uint8_t)(too_big & 0xFF));
    CHECK(cap.count == 0);

    // After the bad lengths, a clean frame still decodes.
    uint8_t frame[rs485::MAX_FRAME_BYTES];
    size_t n = rs485::encode_frame(frame, sizeof(frame), rs485::ADDR_CORE, 5,
                                   rs485::MSG_PING, nullptr, 0);
    for (size_t i = 0; i < n; ++i) rs485::decoder_feed(d, frame[i]);
    CHECK(cap.count == 1);
    rs485::decoder_destroy(d);
}

// The type-byte helper conventions both firmwares rely on.
static void test_type_byte_helpers() {
    std::printf("test_type_byte_helpers\n");
    CHECK(rs485::mk_response(0x10) == 0x90);
    CHECK(rs485::mk_error_response(0x10) == 0xD0);
    CHECK(rs485::is_response(0x90) == true);
    CHECK(rs485::is_response(0x10) == false);
    CHECK(rs485::is_error(0xD0) == true);
    CHECK(rs485::is_error(0x90) == false);   // response but not error
    CHECK(rs485::request_code(0xD0) == 0x10);
}

int main() {
    test_crc_known_answer();
    test_encode_bounds();
    test_encoded_layout();
    test_roundtrip();
    test_resync_after_garbage();
    test_crc_rejection();
    test_bad_length_rejected();
    test_type_byte_helpers();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
