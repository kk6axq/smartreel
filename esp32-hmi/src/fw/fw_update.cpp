#include "fw/fw_update.h"
#include "fw/fw_tag.h"
#include "storage/sdcard.h"

#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>

#include <stdio.h>
#include <string.h>

namespace fw {

// Embedded version tag for this (HMI) image. Scanned out of hmi.bin by
// the same code that reads core.bin, and read directly below for our
// own running version.
FW_TAG_DEFINE("hmi");

const char* version_str() {
    static char s[16];
    snprintf(s, sizeof(s), "%u.%u.%u", g_fw_tag.major, g_fw_tag.minor, g_fw_tag.patch);
    return s;
}

const char* build_str() {
    static char s[sizeof(g_fw_tag.build) + 1];
    memcpy(s, g_fw_tag.build, sizeof(g_fw_tag.build));
    s[sizeof(g_fw_tag.build)] = 0;
    return s;
}

bool file_version(const char* filename, const char* expect_project,
                  char* out, size_t cap) {
    if (!sdcard::mounted()) return false;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", sdcard::mount_point(), filename);
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    // Chunked scan with overlap so a tag straddling a boundary is found.
    const size_t TAG = sizeof(FwTag);
    static uint8_t buf[8192 + sizeof(FwTag)];
    size_t carry = 0;
    bool found = false;
    FwTag tag;
    for (;;) {
        size_t got = fread(buf + carry, 1, 8192, f);
        if (got == 0) break;
        size_t avail = carry + got;
        const FwTag* t = fw_tag_find(buf, avail);
        if (t) { memcpy(&tag, t, sizeof(tag)); found = true; break; }
        carry = (avail >= TAG - 1) ? TAG - 1 : avail;
        memmove(buf, buf + avail - carry, carry);
    }
    fclose(f);
    if (!found) return false;
    if (expect_project &&
        strncmp(tag.project, expect_project, sizeof(tag.project)) != 0) {
        return false;   // tag belongs to the wrong image
    }
    snprintf(out, cap, "%u.%u.%u", tag.major, tag.minor, tag.patch);
    return true;
}

const char* result_str(Result r) {
    switch (r) {
        case Result::Ok:          return "ok";
        case Result::NoCard:      return "no-sd-card";
        case Result::NoFile:      return "file-missing";
        case Result::TooBig:      return "image-too-big";
        case Result::BeginFailed: return "update-begin-failed";
        case Result::WriteFailed: return "write-failed";
        case Result::EndFailed:   return "verify-failed";
        case Result::ReadError:   return "sd-read-error";
    }
    return "?";
}

const char* running_partition_label() {
    const esp_partition_t* p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

const char* running_app_version() {
    const esp_app_desc_t* d = esp_app_get_description();
    return d ? d->version : "?";
}

Result update_hmi_from_sd(const char* filename, ProgressCb cb) {
    if (!sdcard::mounted()) return Result::NoCard;

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", sdcard::mount_point(), filename);

    FILE* f = fopen(path, "rb");
    if (!f) return Result::NoFile;

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (total <= 0) { fclose(f); return Result::NoFile; }

    log_i("fw: OTA from %s (%ld bytes), running=%s",
          path, total, running_partition_label());

    // Update.begin(size) selects the inactive OTA slot automatically and
    // fails if the image won't fit.
    if (!Update.begin((size_t)total, U_FLASH)) {
        log_e("fw: Update.begin failed: %s", Update.errorString());
        fclose(f);
        return Update.getError() == UPDATE_ERROR_SPACE ? Result::TooBig
                                                        : Result::BeginFailed;
    }

    static uint8_t buf[4096];
    size_t done = 0;
    while (done < (size_t)total) {
        size_t want = sizeof(buf);
        if (total - (long)done < (long)want) want = total - done;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) { Update.abort(); fclose(f); return Result::ReadError; }
        if (Update.write(buf, got) != got) {
            log_e("fw: write failed: %s", Update.errorString());
            Update.abort();
            fclose(f);
            return Result::WriteFailed;
        }
        done += got;
        if (cb) cb(done, (size_t)total);
    }
    fclose(f);

    // end(true) validates the image and sets it as the boot partition.
    if (!Update.end(true)) {
        log_e("fw: Update.end failed: %s", Update.errorString());
        return Result::EndFailed;
    }
    log_i("fw: OTA staged ok; reboot to run it");
    return Result::Ok;
}

} // namespace fw
