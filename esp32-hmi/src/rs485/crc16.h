// =====================================================================
//  CRC-16/CCITT-FALSE
//
//  Polynomial: 0x1021
//  Init:       0xFFFF
//  No final XOR, not reflected (input or output).
//
//  This is the standard "CRC-16/IBM-3740" variant used by most binary
//  RS485 protocols where you want a 16-bit check trivially available
//  in any toolchain.
// =====================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len);

// Streaming form: chain multiple calls with the same `crc`. Pass
// 0xFFFF on the first call.
uint16_t crc16_ccitt_false_update(uint16_t crc, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
