// Compact SHA-256 (public-domain style) for verifying RS485 firmware
// images on the RP2040. The ESP32 side uses mbedtls; this is the Core's
// equivalent so FW_VERIFY can compare digests.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const uint8_t* data, size_t len);
void sha256_final(sha256_ctx* c, uint8_t out[32]);   // out = 32-byte digest

#ifdef __cplusplus
}
#endif
