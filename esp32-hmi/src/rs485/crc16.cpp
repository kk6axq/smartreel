#include "rs485/crc16.h"

uint16_t crc16_ccitt_false_update(uint16_t crc, const uint8_t* data, size_t len) {
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len) {
    return crc16_ccitt_false_update(0xFFFF, data, len);
}
