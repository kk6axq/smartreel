// =====================================================================
//  RS485 wire protocol constants -- shared definitions for the
//  ESP32 HMI (bus master) and the RP2040/RP2350 Core (bus slave).
//
//  Keep this header header-only and free of platform includes so the
//  Core firmware can include it verbatim.
//
//  See docs/smartreel-rs485-protocol.md for the design rationale.
// =====================================================================
#pragma once

#include <stdint.h>

namespace rs485 {

// ---- Framing -------------------------------------------------------
static constexpr uint8_t  SYNC0     = 0xAA;
static constexpr uint8_t  SYNC1     = 0x55;

// Maximum payload per frame. With 16-bit LEN we could go much higher,
// but 768 B comfortably covers a FW chunk + framing + a margin, and
// keeps RX buffers reasonable in SRAM.
static constexpr uint16_t MAX_PAYLOAD = 768;

// LEN counts ADDR..end-of-PAYLOAD (so always >= 3: ADDR + SEQ + TYPE)
// and is encoded big-endian.
static constexpr uint16_t MAX_FRAME_BYTES = 2 + 2 + 3 + MAX_PAYLOAD + 2;

// ---- Addresses -----------------------------------------------------
static constexpr uint8_t ADDR_BROADCAST = 0x00;
static constexpr uint8_t ADDR_CORE      = 0x01;   // RP2040/RP2350
static constexpr uint8_t ADDR_MASTER    = 0xFF;   // ESP32 HMI

// ---- Type-byte conventions ----------------------------------------
//   Bit 7 (0x80) -- response/event marker
//   Bit 6 (0x40) -- error marker (set on response only)
//   Bits 0-5     -- request code (high nibble = category, low = op)
//
// Examples: request 0x10 -> response 0x90; error response 0xD0.
static constexpr uint8_t TYPE_RESPONSE_BIT = 0x80;
static constexpr uint8_t TYPE_ERROR_BIT    = 0x40;

constexpr uint8_t mk_response(uint8_t req)        { return req | TYPE_RESPONSE_BIT; }
constexpr uint8_t mk_error_response(uint8_t req)  { return req | TYPE_RESPONSE_BIT | TYPE_ERROR_BIT; }
constexpr bool    is_response(uint8_t t)          { return (t & TYPE_RESPONSE_BIT) != 0; }
constexpr bool    is_error(uint8_t t)             { return (t & (TYPE_RESPONSE_BIT | TYPE_ERROR_BIT)) == (TYPE_RESPONSE_BIT | TYPE_ERROR_BIT); }
constexpr uint8_t request_code(uint8_t t)         { return t & 0x3F; }

// ---- Message catalog ----------------------------------------------
// System (0x0_)
static constexpr uint8_t MSG_PING         = 0x00;
static constexpr uint8_t MSG_GET_VERSION  = 0x01;
static constexpr uint8_t MSG_GET_STATUS   = 0x02;
static constexpr uint8_t MSG_RESET        = 0x03;
static constexpr uint8_t MSG_POLL         = 0x04;
static constexpr uint8_t MSG_ACK_EVENTS   = 0x05;   // ESP32 -> Core, ACKs queued events

// Reel I/O (0x1_)
static constexpr uint8_t MSG_GET_REEL_INFO = 0x10;
static constexpr uint8_t MSG_READ_INPUTS   = 0x11;
static constexpr uint8_t MSG_SET_POLL_RATE = 0x12;

// LED control (0x2_)
static constexpr uint8_t MSG_SET_REEL_PIXELS = 0x20;
static constexpr uint8_t MSG_FILL_REEL       = 0x21;
static constexpr uint8_t MSG_SET_BRIGHTNESS  = 0x22;
static constexpr uint8_t MSG_SET_ANIMATION   = 0x23;
static constexpr uint8_t MSG_COMMIT          = 0x24;

// Async events (0x8_, Core -> ESP32, delivered as POLL response payload)
static constexpr uint8_t EVT_INPUT_CHANGE    = 0x80;
static constexpr uint8_t EVT_REEL_INSERTED   = 0x81;
static constexpr uint8_t EVT_REEL_REMOVED    = 0x82;
static constexpr uint8_t EVT_SENSE_THRESHOLD = 0x83;
static constexpr uint8_t EVT_LOG             = 0x8F;

// Firmware update -- these are REQUEST types, so they must live in the
// 0x00-0x3F request space (bits 6-7 are reserved for the response/error
// markers). The protocol doc's "category F" numbering (0xF0+) is
// unusable here: 0xF0 has both the response (0x80) and error (0x40) bits
// set, so it can neither be sent as a request nor distinguished from an
// error reply. We use the otherwise-unused 0x3_ ("config") category.
static constexpr uint8_t MSG_FW_BEGIN     = 0x30;
static constexpr uint8_t MSG_FW_CHUNK     = 0x31;
static constexpr uint8_t MSG_FW_VERIFY    = 0x32;
static constexpr uint8_t MSG_FW_COMMIT    = 0x33;
static constexpr uint8_t MSG_FW_CONFIRM   = 0x34;
static constexpr uint8_t MSG_FW_BOOTED    = 0x35;   // Core -> ESP32, after a fresh image takes over

// ---- Error codes (carried as the first payload byte in an error
// response) ----------------------------------------------------------
static constexpr uint8_t ERR_OK             = 0x00;
static constexpr uint8_t ERR_UNKNOWN_CMD    = 0x01;
static constexpr uint8_t ERR_BAD_PAYLOAD    = 0x02;
static constexpr uint8_t ERR_REEL_ABSENT    = 0x03;
static constexpr uint8_t ERR_BUSY           = 0x04;   // e.g. flash erase in progress
static constexpr uint8_t ERR_FW_HASH        = 0x10;
static constexpr uint8_t ERR_FW_OOR         = 0x11;   // chunk offset out of range
static constexpr uint8_t ERR_FW_STATE       = 0x12;   // wrong FW state machine state

// ---- Tunables -----------------------------------------------------
// POLL interval. The doc recommends ~20 ms to keep button latency
// under ~50 ms.
static constexpr uint32_t POLL_PERIOD_MS    = 20;

// Per-transaction defaults (the high-level API exposes overrides).
static constexpr uint32_t DEFAULT_TIMEOUT_MS = 50;
static constexpr uint8_t  DEFAULT_RETRIES    = 2;

// Reel id used as "all reels" in commands that accept a bitmap-of-one.
static constexpr uint8_t REEL_ID_ALL = 0xFF;

// ---- Event payload helpers (POLL response layout) -----------------
//
// POLL response payload:
//   count : 1 byte
//   For each event (count copies):
//       type : 1 byte    -- e.g. EVT_INPUT_CHANGE
//       seq  : 1 byte    -- event sequence; ESP32 echoes back via ACK_EVENTS
//       len  : 1 byte    -- length of the event-specific payload
//       data : len bytes
//
// ACK_EVENTS request payload:
//   count : 1 byte
//   seqs  : count bytes -- the event SEQs the ESP32 has handled

} // namespace rs485
