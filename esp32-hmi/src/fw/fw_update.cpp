#include "fw/fw_update.h"
#include "storage/sdcard.h"

#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>

#include <stdio.h>
#include <string.h>

namespace fw {

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
