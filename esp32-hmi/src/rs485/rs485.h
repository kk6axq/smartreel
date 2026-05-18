// =====================================================================
//  RS485 master -- public API
//
//  The ESP32 HMI is the sole bus master. This module owns UART1, the
//  SP3485 DE/RE pin, the SEQ counter, the request mutex, and the
//  background POLL task that drains async events from the Core PCB.
//
//  Threading
//    All public functions are safe to call from any task. Internally
//    a single mutex serialises bus access so request/response
//    transactions don't interleave.
//
//  Errors
//    Most calls return rs485::Status. A nullable out-pointer
//    convention is used for any returned data.
// =====================================================================
#pragma once

#include "rs485/rs485_proto.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace rs485 {

enum class Status : int8_t {
    Ok           =  0,
    Timeout      = -1,   // no reply (or no SEQ match) before deadline
    CrcError     = -2,   // a frame was received but failed CRC
    BadResponse  = -3,   // unexpected type, or error response
    BufferTooSmall = -4, // caller-supplied out buffer too short
    NotReady     = -5,   // init() hasn't been called yet
    Internal     = -6,   // bug / OOM / driver fault
};
const char* status_str(Status s);

// ---- Init ----------------------------------------------------------
// Configures UART1 in half-duplex RS485 mode, allocates RX/TX
// buffers, spawns the POLL task. Idempotent. Returns Ok on success.
Status init();

// ---- Generic transaction ------------------------------------------
// Send a request to `addr` with `type` + payload, wait up to
// timeout_ms for a matching response, retry up to `retries` times on
// timeout. The response payload is copied into `out_payload` and the
// real length written to `*out_payload_len`. Set out=nullptr if you
// don't care about the response body.
//
// If the slave returns an error response, the error code is placed
// in *out_err_code (if non-null) and the call returns
// Status::BadResponse.
Status transact(uint8_t addr, uint8_t type,
                const uint8_t* payload, size_t payload_len,
                uint8_t* out_payload, size_t out_payload_cap,
                size_t*  out_payload_len,
                uint32_t timeout_ms = DEFAULT_TIMEOUT_MS,
                uint8_t  retries    = DEFAULT_RETRIES,
                uint8_t* out_err_code = nullptr);

// ---- High-level convenience wrappers ------------------------------
// System
Status ping(uint8_t addr = ADDR_CORE);

struct CoreVersion {
    uint8_t  fw_major, fw_minor, fw_patch;
    uint32_t build_id;       // 4-byte opaque
    uint8_t  hw_rev;
};
Status get_version(CoreVersion& out, uint8_t addr = ADDR_CORE);

struct CoreStatus {
    uint32_t uptime_s;
    uint8_t  reels_present;  // bitmap; bit N = reel N (0..3)
    uint16_t error_flags;
};
Status get_status(CoreStatus& out, uint8_t addr = ADDR_CORE);

enum class ResetReason : uint8_t {
    Normal          = 0,
    EnterUpdateMode = 1,
};
Status reset_core(ResetReason reason = ResetReason::Normal, uint8_t addr = ADDR_CORE);

// Reel I/O
struct ReelInfo {
    bool     present;
    uint16_t sense_mv;
    uint8_t  id_bytes[8];    // truncated if Core returns fewer
    uint8_t  id_len;
};
// reel_id = 0..3, or REEL_ID_ALL to fetch all in one call. When
// fetching all, fills `infos[0..N-1]` and writes the count to *n_out.
Status get_reel_info(uint8_t reel_id, ReelInfo* infos, int max_infos, int* n_out,
                     uint8_t addr = ADDR_CORE);

// Reads the PISO shift-register state (one or more bytes; Core
// returns whatever its hardware gives).
Status read_inputs(uint8_t reel_id, uint8_t* out, size_t out_cap, size_t* out_len,
                   uint8_t addr = ADDR_CORE);

Status set_poll_rate(uint8_t reel_id, uint16_t rate_hz, uint8_t addr = ADDR_CORE);

// LED control
Status set_reel_pixels(uint8_t reel_id, uint16_t start_idx,
                       const uint8_t* rgb_triples, size_t triple_count,
                       uint8_t addr = ADDR_CORE);
Status fill_reel(uint8_t reel_id, uint8_t r, uint8_t g, uint8_t b,
                 uint8_t addr = ADDR_CORE);
Status set_brightness(uint8_t reel_id, uint8_t brightness,
                      uint8_t addr = ADDR_CORE);
Status set_animation(uint8_t reel_id, uint8_t anim_id,
                     const uint8_t* params, size_t params_len,
                     uint8_t addr = ADDR_CORE);
// reel_bitmap: bit N = include reel N in the atomic show()
Status commit_pixels(uint8_t reel_bitmap, uint8_t addr = ADDR_CORE);

// ---- Async events --------------------------------------------------
// Events arrive in POLL responses. The POLL task copies them out of
// the response frame, ACKs them back to the Core, and dispatches
// each to the registered handler.
struct Event {
    uint8_t       type;       // EVT_INPUT_CHANGE, EVT_REEL_INSERTED, ...
    uint8_t       seq;        // Core-assigned; auto-ACK'd by the POLL task
    const uint8_t* payload;   // pointer into the POLL RX buffer; copy if you keep it
    uint8_t       payload_len;
};
typedef void (*EventHandler)(const Event& e, void* user);

// Register a single handler. Pass nullptr to clear. Replaces any
// previous registration.
void set_event_handler(EventHandler cb, void* user);

// ---- Diagnostics ---------------------------------------------------
struct Stats {
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t crc_errors;
    uint32_t timeouts;
    uint32_t retries;
    uint32_t events_received;
};
const Stats& stats();

// Pause / resume the background POLL task. Used during firmware
// updates where the bus needs to be exclusive.
void poll_pause();
void poll_resume();

} // namespace rs485
