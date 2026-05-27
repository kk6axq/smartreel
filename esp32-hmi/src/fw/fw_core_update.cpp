#include "fw/fw_core_update.h"
#include "storage/sdcard.h"
#include "rs485/rs485.h"
#include "rs485/rs485_proto.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include <stdio.h>
#include <string.h>

namespace fw {

using namespace rs485;

// FW_CHUNK data payload size. Frame = 4 (offset) + data; keep under
// MAX_PAYLOAD (768) with margin.
static constexpr size_t CHUNK = 512;

const char* core_result_str(CoreResult r) {
    switch (r) {
        case CoreResult::Ok:           return "ok";
        case CoreResult::NoCard:       return "no-sd-card";
        case CoreResult::NoFile:       return "file-missing";
        case CoreResult::Oom:          return "out-of-memory";
        case CoreResult::BeginFailed:  return "begin-rejected";
        case CoreResult::ChunkFailed:  return "chunk-failed";
        case CoreResult::VerifyFailed: return "verify-mismatch";
        case CoreResult::CommitFailed: return "commit-rejected";
    }
    return "?";
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static uint32_t get_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

CoreResult update_core_from_sd(const char* filename, ProgressCb cb) {
    if (!sdcard::mounted()) return CoreResult::NoCard;

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", sdcard::mount_point(), filename);
    FILE* f = fopen(path, "rb");
    if (!f) return CoreResult::NoFile;
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (total <= 0) { fclose(f); return CoreResult::NoFile; }

    // Buffer the whole image (PSRAM); Core images are small.
    uint8_t* img = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!img) img = (uint8_t*)malloc(total);
    if (!img) { fclose(f); return CoreResult::Oom; }
    size_t rd = fread(img, 1, total, f);
    fclose(f);
    if (rd != (size_t)total) { free(img); return CoreResult::NoFile; }

    uint8_t sha[32];
    mbedtls_sha256(img, total, sha, 0);   // 0 => SHA-256 (not 224)
    log_i("fw-core: %s (%ld bytes), pushing over RS485", path, total);

    CoreResult result = CoreResult::Ok;
    poll_pause();
    delay(5);   // let an in-flight POLL finish

    do {
        // FW_BEGIN: total_size(4) + sha256(32) + version(4)
        uint8_t begin[40];
        put_be32(begin, (uint32_t)total);
        memcpy(begin + 4, sha, 32);
        put_be32(begin + 36, 0);   // version: unused on the rig
        if (transact(ADDR_CORE, MSG_FW_BEGIN, begin, sizeof(begin),
                     nullptr, 0, nullptr, 5000, 2) != Status::Ok) {
            result = CoreResult::BeginFailed; break;
        }

        // FW_CHUNK loop. The Core ACKs with bytes-received-so-far.
        uint8_t pkt[4 + CHUNK];
        uint8_t ack[8]; size_t acklen = 0;
        uint32_t offset = 0;
        while (offset < (uint32_t)total) {
            size_t dlen = total - offset;
            if (dlen > CHUNK) dlen = CHUNK;
            put_be32(pkt, offset);
            memcpy(pkt + 4, img + offset, dlen);
            Status s = transact(ADDR_CORE, MSG_FW_CHUNK, pkt, 4 + dlen,
                                ack, sizeof(ack), &acklen, 500, 4);
            if (s != Status::Ok || acklen < 4) { result = CoreResult::ChunkFailed; break; }
            uint32_t received = get_be32(ack);
            if (received <= offset) { result = CoreResult::ChunkFailed; break; }
            offset = received;
            if (cb) cb(offset, (size_t)total);
        }
        if (result != CoreResult::Ok) break;

        // FW_VERIFY
        if (transact(ADDR_CORE, MSG_FW_VERIFY, nullptr, 0,
                     nullptr, 0, nullptr, 2000, 2) != Status::Ok) {
            result = CoreResult::VerifyFailed; break;
        }

        // FW_COMMIT -- Core acks then reboots, so retries=0.
        if (transact(ADDR_CORE, MSG_FW_COMMIT, nullptr, 0,
                     nullptr, 0, nullptr, 500, 0) != Status::Ok) {
            result = CoreResult::CommitFailed; break;
        }
    } while (0);

    free(img);
    poll_resume();
    return result;
}

} // namespace fw
