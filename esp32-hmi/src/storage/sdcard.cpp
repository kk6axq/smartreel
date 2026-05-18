// =====================================================================
//  microSD - raw ESP-IDF SDMMC mount.
//
//  We talk to the SD/MMC peripheral directly (instead of going through
//  Arduino's SD_MMC class) for two reasons:
//    1. We need the sdmmc_card_t* to call esp_vfs_fat_sdmmc_format()
//       for the maintenance "wipe & re-format" function.
//    2. The mount config exposes `format_if_mount_failed`, so a card
//       with an unknown filesystem (exFAT, NTFS, blank) auto-formats
//       to FAT32 on first insert instead of the mount silently
//       failing.
//
//  The mount registers a VFS at "/sdcard", so all file I/O goes
//  through standard POSIX (fopen/fread/fwrite/rename).
// =====================================================================
#include "storage/sdcard.h"
#include "board/board_pins.h"

#include <Arduino.h>
#include <esp32-hal-log.h>
#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace sdcard {

static bool             s_mounted = false;
static sdmmc_card_t*    s_card    = nullptr;
static constexpr const char* MOUNT = "/sdcard";

bool init() {
    if (s_mounted) return true;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags        = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk   = (gpio_num_t)SD_MMC_CLK;
    slot.cmd   = (gpio_num_t)SD_MMC_CMD;
    slot.d0    = (gpio_num_t)SD_MMC_D0;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = true;     // auto-format unknown FS
    mount_cfg.max_files              = 5;
    mount_cfg.allocation_unit_size   = 16 * 1024;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            log_w("SD: mount failed (no card or unrecognised media)");
        } else {
            log_w("SD: mount error 0x%x (%s)", err, esp_err_to_name(err));
        }
        s_mounted = false;
        s_card    = nullptr;
        return false;
    }

    log_i("SD card mounted: %llu MB",
          ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024ULL * 1024ULL));
    s_mounted = true;
    return true;
}

bool mounted()                  { return s_mounted; }
const char* mount_point()       { return MOUNT; }

unsigned long long capacity_bytes() {
    if (!s_mounted || !s_card) return 0;
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

size_t file_size(const char* path) {
    if (!s_mounted) return 0;
    struct stat st;
    if (::stat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode))   return 0;
    return (size_t)st.st_size;
}

size_t read_file(const char* path, void* buf, size_t buf_len) {
    if (!s_mounted || !buf || buf_len == 0) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(buf, 1, buf_len, f);
    fclose(f);
    return got;
}

bool write_file_atomic(const char* path, const void* buf, size_t len) {
    if (!s_mounted || !buf) return false;

    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* f = fopen(tmp, "wb");
    if (!f) {
        log_e("SD: cannot open %s for write (errno %d)", tmp, errno);
        return false;
    }
    size_t wrote = fwrite(buf, 1, len, f);
    fflush(f);
    fclose(f);
    if (wrote != len) {
        log_e("SD: short write %u/%u to %s", (unsigned)wrote, (unsigned)len, tmp);
        ::remove(tmp);
        return false;
    }
    // Replace the destination. ::rename is atomic on FAT.
    ::remove(path);   // ignore error: target may not exist
    if (::rename(tmp, path) != 0) {
        log_e("SD: rename %s -> %s failed (errno %d)", tmp, path, errno);
        ::remove(tmp);
        return false;
    }
    return true;
}

bool format() {
    if (!s_mounted || !s_card) {
        log_w("format: no card mounted; running init() (auto-formats unknown FS)");
        return init();
    }
    log_w("SD: formatting card (this can take several seconds)...");
    esp_err_t err = esp_vfs_fat_sdcard_format(MOUNT, s_card);
    if (err != ESP_OK) {
        log_e("SD format failed: 0x%x (%s)", err, esp_err_to_name(err));
        return false;
    }
    log_i("SD format complete");
    return true;
}

} // namespace sdcard
