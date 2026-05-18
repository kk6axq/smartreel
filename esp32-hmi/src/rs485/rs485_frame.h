// =====================================================================
//  Frame encoder + streaming decoder for the SmartReel RS485 link.
//
//  Encoder: build a frame into a caller-supplied buffer.
//  Decoder: feed bytes one at a time; when a complete, CRC-valid
//           frame arrives, on_frame() is called.
// =====================================================================
#pragma once

#include "rs485/rs485_proto.h"
#include <stdbool.h>
#include <stddef.h>

namespace rs485 {

// ---- Encoder -------------------------------------------------------
// Build a complete frame (sync ... CRC) into `out`.
//
//   out     : buffer at least MAX_FRAME_BYTES long
//   out_cap : capacity of `out`
//   addr    : destination address (ADDR_CORE, ADDR_BROADCAST, ...)
//   seq     : caller-managed sequence number (response will echo it)
//   type    : message type byte (e.g. MSG_PING)
//   payload : may be nullptr if payload_len == 0
//
// Returns the number of bytes written, or 0 if `out_cap` is too small
// or `payload_len` exceeds MAX_PAYLOAD.
size_t encode_frame(uint8_t* out, size_t out_cap,
                    uint8_t addr, uint8_t seq, uint8_t type,
                    const uint8_t* payload, size_t payload_len);

// ---- Decoder (streaming) ------------------------------------------
struct Decoder; // opaque; defined in the .cpp

// Callback invoked when a fully validated frame is received. The
// payload pointer is valid only for the duration of the call; copy
// what you need before returning.
typedef void (*FrameCallback)(uint8_t addr, uint8_t seq, uint8_t type,
                              const uint8_t* payload, size_t payload_len,
                              void* user);

Decoder*  decoder_create(FrameCallback cb, void* user);
void      decoder_destroy(Decoder* d);

// Feed one byte. Calls `cb` (registered at create-time) once a
// validated frame is assembled. Robust to garbage; will resync on
// the next 0xAA 0x55.
void      decoder_feed(Decoder* d, uint8_t byte);

// Reset state (e.g. after a long silence on the bus).
void      decoder_reset(Decoder* d);

} // namespace rs485
