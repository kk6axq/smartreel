// =====================================================================
//  Embedded firmware version tag.
//
//  Both firmwares bake one FwTag into their image (FW_TAG_DEFINE). It's
//  a small magic-prefixed struct in .rodata, so the running firmware can
//  read its own version AND the HMI can recover the version of an update
//  image sitting on the SD card by scanning the .bin for the magic.
//
//  Version numbers come from -DFW_VER_MAJOR/MINOR/PATCH (set per project
//  in platformio.ini). Shared verbatim by the ESP32 and RP2040 builds.
// =====================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#ifndef FW_VER_MAJOR
#define FW_VER_MAJOR 0
#endif
#ifndef FW_VER_MINOR
#define FW_VER_MINOR 0
#endif
#ifndef FW_VER_PATCH
#define FW_VER_PATCH 0
#endif

struct FwTag {
    char    magic[8];      // 'S','R','F','W','T','A','G','1'
    uint8_t major, minor, patch, reserved;
    char    project[8];    // "hmi" / "core", nul-padded
    char    build[24];     // __DATE__ " " __TIME__
};

// Define the (single) tag instance for an image. Use once per project,
// in a .cpp, passing the project name string ("hmi" / "core").
#define FW_TAG_DEFINE(projname)                                        \
    const FwTag g_fw_tag __attribute__((used)) = {                     \
        { 'S','R','F','W','T','A','G','1' },                           \
        (uint8_t)FW_VER_MAJOR, (uint8_t)FW_VER_MINOR,                  \
        (uint8_t)FW_VER_PATCH, 0,                                      \
        projname, __DATE__ " " __TIME__ }

// Scan a buffer for the magic; returns a pointer to the tag or nullptr.
// (The tag may straddle chunk boundaries -- callers reading a file in
// chunks must overlap reads by at least sizeof(FwTag)-1 bytes.)
//
// The magic is matched against byte literals rather than a stored
// string so this function does NOT itself emit "SRFWTAG1" into .rodata
// -- otherwise an image that both defines a tag and scans (the HMI)
// would contain the magic twice and a scan could match the wrong copy.
static inline const FwTag* fw_tag_find(const uint8_t* b, size_t len) {
    if (!b || len < sizeof(FwTag)) return nullptr;
    for (size_t i = 0; i + sizeof(FwTag) <= len; ++i) {
        if (b[i]   == 'S' && b[i+1] == 'R' && b[i+2] == 'F' && b[i+3] == 'W' &&
            b[i+4] == 'T' && b[i+5] == 'A' && b[i+6] == 'G' && b[i+7] == '1')
            return (const FwTag*)(b + i);
    }
    return nullptr;
}
