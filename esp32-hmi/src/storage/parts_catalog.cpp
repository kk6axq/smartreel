#include "storage/parts_catalog.h"
#include "storage/sdcard.h"

#include <ArduinoJson.h>
#include <esp32-hal-log.h>
#include <string.h>
#include <stdio.h>

namespace parts_catalog {

static constexpr const char* PATH_PARTS = "/sdcard/parts.json";
static constexpr int MAX_PARTS = 64;

struct Entry {
    char       qr[24];     // "MOCK-PART-00001"
    app::Part  part;
};

static Entry s_parts[MAX_PARTS];
static int   s_count = 0;

int count() { return s_count; }

const char* qr_at(int i) {
    if (i < 0 || i >= s_count) return nullptr;
    return s_parts[i].qr;
}

bool lookup(const char* qr, app::Part& out) {
    if (!qr) return false;
    for (int i = 0; i < s_count; ++i) {
        if (strcmp(s_parts[i].qr, qr) == 0) {
            out = s_parts[i].part;
            return true;
        }
    }
    return false;
}

bool load() {
    s_count = 0;
    if (!sdcard::mounted()) {
        log_i("parts_catalog: no SD; catalog empty");
        return false;
    }
    size_t fsz = sdcard::file_size(PATH_PARTS);
    if (fsz == 0) {
        log_i("parts_catalog: %s missing; catalog empty", PATH_PARTS);
        return false;
    }
    if (fsz > 16 * 1024) {
        log_w("parts_catalog: %s too large (%u); skipping", PATH_PARTS, (unsigned)fsz);
        return false;
    }

    char* buf = static_cast<char*>(malloc(fsz + 1));
    if (!buf) return false;
    size_t got = sdcard::read_file(PATH_PARTS, buf, fsz);
    buf[got] = 0;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, got);
    free(buf);
    if (err) {
        log_w("parts.json parse failed: %s", err.c_str());
        return false;
    }

    JsonArrayConst parts = doc["parts"].as<JsonArrayConst>();
    for (JsonObjectConst o : parts) {
        if (s_count >= MAX_PARTS) break;
        const char* qr = o["qr"] | "";
        if (!qr[0]) continue;
        Entry& e = s_parts[s_count++];
        snprintf(e.qr,        sizeof(e.qr),        "%s", qr);
        snprintf(e.part.id,   sizeof(e.part.id),   "%s", o["id"]   | "");
        snprintf(e.part.name, sizeof(e.part.name), "%s", o["name"] | "");
        snprintf(e.part.pkg,  sizeof(e.part.pkg),  "%s", o["pkg"]  | "");
        snprintf(e.part.mfg,  sizeof(e.part.mfg),  "%s", o["mfg"]  | "");
        e.part.valid = true;
    }
    log_i("parts.json loaded (%d parts)", s_count);
    return s_count > 0;
}

} // namespace parts_catalog
