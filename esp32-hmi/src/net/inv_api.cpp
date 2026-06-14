#include "net/inv_api.h"

#include "storage/config_store.h"
#include "net/wifi_mgr.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp32-hal-log.h>
#include <string.h>
#include <stdio.h>

namespace inv_api {

namespace {

constexpr int   HTTP_TIMEOUT_MS = 4000;
constexpr int   HTTP_CONN_TIMEOUT_MS = 2000;

// Cached "last contact" state. Updated under no lock -- single-writer
// from whichever worker task last completed a call, reader is UI on
// LVGL task; torn reads here only mean stale text on one frame.
HealthResult g_last_health = {};
bool         g_ever_ok     = false;
uint32_t     g_last_ok_ms  = 0;
uint32_t     g_op_counter  = 0;
uint64_t     g_chip_id     = 0;

void capture_success() {
    g_ever_ok = true;
    g_last_ok_ms = millis();
}

// Forward decls so do_request can call into the shared helpers below.
int  finish_request(HTTPClient& http, const char* method,
                    const char* body_json, String& resp_out);

// Append /api/v1/<tail> to whatever URL the user typed, tolerating a
// trailing slash. Returns false if the URL is empty, the buffer is too
// small, or the scheme is not https:// -- the HMI is HTTPS-only by
// design (the dev mock server defaults to TLS too); plain http is
// rejected here rather than at request time so we never put auth
// tokens on the wire in cleartext.
bool build_url(char* out, size_t out_size, const char* tail) {
    const auto& url = config_store::cfg().inventree.url;
    if (!url[0]) return false;
    if (strncmp(url, "https://", 8) != 0) return false;
    size_t n = strlen(url);
    bool trailing_slash = (n > 0 && url[n - 1] == '/');
    int written = snprintf(out, out_size, "%s%sapi/v1/%s",
                           url, trailing_slash ? "" : "/", tail);
    return written > 0 && (size_t)written < out_size;
}

// Surface a precise error when the URL is missing or not https. Used
// by every public call so the UI doesn't have to repeat the logic.
bool url_error(char* err, size_t err_size) {
    const auto& url = config_store::cfg().inventree.url;
    if (!url[0]) {
        snprintf(err, err_size, "no URL configured");
        return true;
    }
    if (strncmp(url, "https://", 8) != 0) {
        snprintf(err, err_size, "URL must start with https://");
        return true;
    }
    return false;
}

// One-shot HTTPClient call. Body may be nullptr for GET. HTTPS only:
// the HMI never speaks plain HTTP to the plugin (cleartext auth token
// would leak on any path). build_url() rejects non-https URLs before
// we get here; this is the second line of defence.
//
//   Returns:
//     positive  -- HTTP status code; resp_out filled with body
//     0         -- transport failure; resp_out left empty
//
// Timeouts are conservative because the LVGL UI shouldn't hang for
// more than a few seconds even on a totally cold server.
int do_request(const char* method, const char* full_url,
               const char* body_json, String& resp_out) {
    if (strncmp(full_url, "https://", 8) != 0) {
        log_w("inv_api: refusing non-https URL: %s", full_url);
        return 0;
    }
    // NetworkClientSecure must outlive HTTPClient::end(), so keep it
    // on the stack and never move.
    NetworkClientSecure tls;
    tls.setInsecure();   // dev: self-signed cert; cert pinning is TODO
    tls.setTimeout(HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    if (!http.begin(tls, full_url)) {
        log_w("inv_api: http.begin failed: %s", full_url);
        return 0;
    }
    return finish_request(http, method, body_json, resp_out);
}

// Shared by do_request's https / http branches. Mutates `http`, owns
// none of it; the caller calls http.end() (via this fn) and lets its
// own TLS/TCP client go out of scope.
int finish_request(HTTPClient& http, const char* method,
                   const char* body_json, String& resp_out) {
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_CONN_TIMEOUT_MS);
    http.setReuse(false);

    // Auth header on every call. /health doesn't require it but the
    // server tolerates extras.
    char auth[128];
    snprintf(auth, sizeof(auth), "Token %s", config_store::cfg().inventree.token);
    http.addHeader("Authorization", auth);
    if (body_json) http.addHeader("Content-Type", "application/json");

    int code = 0;
    if (strcmp(method, "GET") == 0) {
        code = http.GET();
    } else if (strcmp(method, "POST") == 0) {
        code = http.POST(body_json ? body_json : "");
    } else {
        log_w("inv_api: unsupported method %s", method);
        http.end();
        return 0;
    }

    if (code > 0) {
        resp_out = http.getString();
    } else {
        log_w("inv_api: transport error: %d (%s)", code, http.errorToString(code).c_str());
    }
    http.end();
    return code > 0 ? code : 0;
}

// Translate the HTTP outcome into our Status enum + error string.
// Always returns through `out_status / out_http / out_err`.
void classify(int http_code, const String& body,
              Status& out_status, int& out_http, char* out_err, size_t err_size) {
    out_http = http_code;
    if (http_code <= 0) {
        out_status = Status::NetworkError;
        snprintf(out_err, err_size, "network unreachable");
        return;
    }
    if (http_code >= 200 && http_code < 300) {
        out_status = Status::Ok;
        out_err[0] = 0;
        return;
    }
    out_status = Status::BadStatus;
    // Try to pull a "detail" field out of the JSON body for the UI; fall
    // back to "HTTP <code>" if the body isn't JSON or is huge.
    if (body.length() > 0 && body.length() < 512) {
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            const char* detail = doc["detail"] | "";
            if (detail[0]) {
                snprintf(out_err, err_size, "HTTP %d: %s", http_code, detail);
                return;
            }
        }
    }
    snprintf(out_err, err_size, "HTTP %d", http_code);
}

// Lift fields off a JsonObjectConst into a Part struct.
void load_part(Part& p, JsonObjectConst o) {
    snprintf(p.id,   sizeof(p.id),   "%s", o["id"]   | "");
    snprintf(p.name, sizeof(p.name), "%s", o["name"] | "");
    snprintf(p.pkg,  sizeof(p.pkg),  "%s", o["pkg"]  | "");
    snprintf(p.mfg,  sizeof(p.mfg),  "%s", o["mfg"]  | "");
}

void load_stock(Stock& s, JsonObjectConst o) {
    s.id = o["id"] | 0;
    load_part(s.part, o["part"].as<JsonObjectConst>());
    s.qty         = o["qty"] | 0;
    snprintf(s.batch,   sizeof(s.batch),   "%s", o["batch"]   | "");
    snprintf(s.barcode, sizeof(s.barcode), "%s", o["barcode"] | "");
    s.location_id = o["location_id"] | 0;
    s.slot_num    = o["slot_num"]    | 0;
}

} // anonymous namespace

// ---- Public ---------------------------------------------------------

const char* status_str(Status s) {
    switch (s) {
        case Status::Ok:            return "ok";
        case Status::NotConfigured: return "not configured";
        case Status::NoWifi:        return "no wifi";
        case Status::NetworkError:  return "network error";
        case Status::BadStatus:     return "bad status";
        case Status::ParseError:    return "parse error";
        case Status::Timeout:       return "timeout";
    }
    return "?";
}

void init() {
    g_chip_id = ESP.getEfuseMac();
    memset(&g_last_health, 0, sizeof(g_last_health));
    g_last_health.status = Status::NotConfigured;
}

bool configured() {
    if (wifi_mgr::status() != wifi_mgr::State::Connected) return false;
    const auto& iv = config_store::cfg().inventree;
    return iv.url[0] && iv.token[0];
}

void make_op_id(char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    uint32_t n = ++g_op_counter;
    // Short chip id (low 24 bits) keeps the op_id readable in logs.
    uint32_t chip = (uint32_t)(g_chip_id & 0xFFFFFF);
    snprintf(out, out_size, "hmi-%06x-%lu-%lu",
             (unsigned)chip,
             (unsigned long)millis(),
             (unsigned long)n);
}

const HealthResult& last_health()  { return g_last_health; }
bool                ever_succeeded() { return g_ever_ok; }
uint32_t            last_success_ms() { return g_last_ok_ms; }

// ---- health ---------------------------------------------------------

HealthResult health() {
    HealthResult r{};
    r.status = Status::NotConfigured;
    if (wifi_mgr::status() != wifi_mgr::State::Connected) {
        r.status = Status::NoWifi;
        snprintf(r.error, sizeof(r.error), "wifi not connected");
        g_last_health = r;
        return r;
    }
    if (url_error(r.error, sizeof(r.error))) {
        g_last_health = r;
        return r;
    }

    char url[160];
    if (!build_url(url, sizeof(url), "health")) {
        r.status = Status::NotConfigured;
        snprintf(r.error, sizeof(r.error), "URL too long");
        g_last_health = r;
        return r;
    }

    String body;
    int code = do_request("GET", url, nullptr, body);
    classify(code, body, r.status, r.http_code, r.error, sizeof(r.error));

    if (r.status == Status::Ok) {
        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            r.status = Status::ParseError;
            snprintf(r.error, sizeof(r.error), "bad health json");
        } else {
            snprintf(r.server,  sizeof(r.server),  "%s", doc["server"]  | "");
            snprintf(r.version, sizeof(r.version), "%s", doc["version"] | "");
            capture_success();
        }
    }
    g_last_health = r;
    return r;
}

// ---- resolve_barcode ------------------------------------------------

ResolveResult resolve_barcode(const char* code, const char* op_id) {
    ResolveResult r{};
    r.type   = ResolveType::Unknown;
    r.status = Status::NotConfigured;
    if (!code || !code[0]) {
        snprintf(r.error, sizeof(r.error), "empty code");
        return r;
    }
    if (wifi_mgr::status() != wifi_mgr::State::Connected) {
        r.status = Status::NoWifi;
        snprintf(r.error, sizeof(r.error), "wifi not connected");
        return r;
    }
    if (url_error(r.error, sizeof(r.error))) return r;
    if (!config_store::cfg().inventree.token[0]) {
        snprintf(r.error, sizeof(r.error), "no token configured");
        return r;
    }

    char url[160];
    if (!build_url(url, sizeof(url), "barcode/resolve")) {
        snprintf(r.error, sizeof(r.error), "URL too long");
        return r;
    }

    // Sized for a long QR/DataMatrix payload plus the op_id; checked for
    // truncation so an oversized code fails cleanly instead of sending a
    // malformed JSON body that the server would reject anyway.
    char body_buf[320];
    {
        JsonDocument req;
        req["code"] = code;
        if (op_id && op_id[0]) req["op_id"] = op_id;
        if (serializeJson(req, body_buf, sizeof(body_buf)) >= sizeof(body_buf)) {
            r.status = Status::BadStatus;
            snprintf(r.error, sizeof(r.error), "scanned code too long");
            return r;
        }
    }

    String body;
    int code_http = do_request("POST", url, body_buf, body);
    classify(code_http, body, r.status, r.http_code, r.error, sizeof(r.error));
    if (r.status != Status::Ok) return r;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        r.status = Status::ParseError;
        snprintf(r.error, sizeof(r.error), "bad resolve json");
        return r;
    }

    const char* type_str = doc["type"] | "unknown";
    if      (strcmp(type_str, "stockitem") == 0) r.type = ResolveType::StockItem;
    else if (strcmp(type_str, "part")      == 0) r.type = ResolveType::ItemPart;
    else if (strcmp(type_str, "location")  == 0) r.type = ResolveType::Location;
    else                                         r.type = ResolveType::Unknown;

    if (r.type == ResolveType::StockItem) {
        load_stock(r.stock, doc["stock"].as<JsonObjectConst>());
    } else if (r.type == ResolveType::ItemPart) {
        load_part(r.part, doc["part"].as<JsonObjectConst>());
    }
    snprintf(r.message, sizeof(r.message), "%s", doc["message"] | "");
    capture_success();
    return r;
}

// ---- shared helper for the slot mutators ----------------------------

static SlotMutResult slot_mut_call(const char* path, const char* body_buf) {
    SlotMutResult r{};
    if (wifi_mgr::status() != wifi_mgr::State::Connected) {
        r.status = Status::NoWifi;
        snprintf(r.error, sizeof(r.error), "wifi not connected");
        return r;
    }
    if (url_error(r.error, sizeof(r.error))) {
        r.status = Status::NotConfigured;
        return r;
    }
    if (!config_store::cfg().inventree.token[0]) {
        r.status = Status::NotConfigured;
        snprintf(r.error, sizeof(r.error), "no token configured");
        return r;
    }
    char url[160];
    if (!build_url(url, sizeof(url), path)) {
        r.status = Status::NotConfigured;
        snprintf(r.error, sizeof(r.error), "URL too long");
        return r;
    }
    String body;
    int code = do_request("POST", url, body_buf, body);
    classify(code, body, r.status, r.http_code, r.error, sizeof(r.error));
    if (r.status != Status::Ok) return r;

    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
        r.stock_id = doc["stock_id"] | 0;
        r.moved_to = doc["moved_to"] | 0;
    }
    capture_success();
    return r;
}

// ---- mutators -------------------------------------------------------

SlotMutResult assign_slot(int slot_num, int stock_item_id, const char* op_id) {
    char path[40], body[80];
    snprintf(path, sizeof(path), "rack/slots/%d/assign", slot_num);
    JsonDocument req;
    req["stock_item_id"] = stock_item_id;
    req["op_id"]         = op_id ? op_id : "";
    serializeJson(req, body, sizeof(body));
    return slot_mut_call(path, body);
}

SlotMutResult pick_slot(int slot_num, const char* op_id) {
    char path[40], body[64];
    snprintf(path, sizeof(path), "rack/slots/%d/pick", slot_num);
    JsonDocument req;
    req["op_id"] = op_id ? op_id : "";
    serializeJson(req, body, sizeof(body));
    return slot_mut_call(path, body);
}

SlotMutResult clear_slot(int slot_num, const char* reason, const char* op_id) {
    char path[40], body[96];
    snprintf(path, sizeof(path), "rack/slots/%d/clear", slot_num);
    JsonDocument req;
    req["op_id"]  = op_id  ? op_id  : "";
    if (reason && reason[0]) req["reason"] = reason;
    serializeJson(req, body, sizeof(body));
    return slot_mut_call(path, body);
}

SlotMutResult report_anomaly(const char* kind, int slot_num,
                             const char* detail, const char* op_id) {
    char body[256];
    JsonDocument req;
    req["kind"]     = kind ? kind : "removed";
    req["slot_num"] = slot_num;
    if (detail && detail[0]) req["detail"] = detail;
    req["op_id"]    = op_id ? op_id : "";
    serializeJson(req, body, sizeof(body));
    return slot_mut_call("anomaly", body);
}

SlotMutResult register_rack(int n_slots, const char* op_id) {
    char body[64];
    JsonDocument req;
    req["n_slots"] = n_slots;
    req["op_id"]   = op_id ? op_id : "";
    serializeJson(req, body, sizeof(body));
    return slot_mut_call("rack/register", body);
}

SlotMutResult ack_locates(const int* ids, int n_ids) {
    // {"ids":[...]}; n_ids == 0 sends {"ids":[]} which clears the queue.
    // No op_id: locate ids originate in InvenTree and ack is idempotent by
    // set-difference, so the op_id replay-cache buys us nothing here.
    // Body sized for the plugin's 50-id cap (worst case ~9 chars/id incl.
    // separator) plus the wrapper, with truncation checked below.
    char body[512];
    {
        JsonDocument req;
        JsonArray arr = req["ids"].to<JsonArray>();
        for (int i = 0; i < n_ids; ++i) arr.add(ids[i]);
        if (serializeJson(req, body, sizeof(body)) >= sizeof(body)) {
            SlotMutResult r{};
            r.status = Status::BadStatus;
            snprintf(r.error, sizeof(r.error), "too many locate ids");
            return r;
        }
    }
    return slot_mut_call("rack/locates/ack", body);
}

// ---- GET /rack -------------------------------------------------------

void get_rack(RackResult& out) {
    out.status = Status::NotConfigured;
    out.n_slots = 0;
    if (wifi_mgr::status() != wifi_mgr::State::Connected) {
        out.status = Status::NoWifi;
        snprintf(out.error, sizeof(out.error), "wifi not connected");
        return;
    }
    if (url_error(out.error, sizeof(out.error))) return;

    char url[160];
    if (!build_url(url, sizeof(url), "rack")) {
        snprintf(out.error, sizeof(out.error), "URL too long");
        return;
    }

    String body;
    int code = do_request("GET", url, nullptr, body);
    classify(code, body, out.status, out.http_code, out.error, sizeof(out.error));
    if (out.status != Status::Ok) return;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        out.status = Status::ParseError;
        snprintf(out.error, sizeof(out.error), "bad rack json");
        return;
    }
    out.location_id         = doc["location_id"] | 0;
    out.pickjobs_available  = doc["pickjobs_available"] | 0;
    out.n_locates           = 0;

    for (JsonObjectConst s : doc["slots"].as<JsonArrayConst>()) {
        if (out.n_slots >= RackResult::MAX_SLOTS) break;
        RackSlot& rs = out.slots[out.n_slots++];
        rs.slot = s["slot"] | 0;
        JsonObjectConst stock = s["stock"].as<JsonObjectConst>();
        rs.occupied = !stock.isNull();
        if (rs.occupied) {
            rs.stock_id = stock["id"]  | 0;
            rs.qty      = stock["qty"] | 0;
            load_part(rs.part, stock["part"].as<JsonObjectConst>());
        } else {
            rs.stock_id = 0;
            rs.qty      = 0;
            rs.part     = Part{};
        }
    }

    // Pending locate requests (web-UI "locate" button). Empty array when
    // none; bounded to MAX_LOCATES (the plugin caps its queue at 50, so we
    // size to match -- extras are dropped, but the ack/dedupe path still
    // drains what we did light on subsequent polls).
    for (JsonObjectConst l : doc["locates"].as<JsonArrayConst>()) {
        if (out.n_locates >= RackResult::MAX_LOCATES) break;
        Locate& loc = out.locates[out.n_locates++];
        loc.id       = l["id"]       | 0;
        loc.slot_num = l["slot_num"] | 0;
    }
    capture_success();
}

// ---- GET /pickjobs ----------------------------------------------------

void get_pickjobs(PickJobsResult& out) {
    out.status = Status::NotConfigured;
    out.n_jobs = 0;
    if (wifi_mgr::status() != wifi_mgr::State::Connected) {
        out.status = Status::NoWifi;
        snprintf(out.error, sizeof(out.error), "wifi not connected");
        return;
    }
    if (url_error(out.error, sizeof(out.error))) return;

    char url[160];
    if (!build_url(url, sizeof(url), "pickjobs")) {
        snprintf(out.error, sizeof(out.error), "URL too long");
        return;
    }

    String body;
    int code = do_request("GET", url, nullptr, body);
    classify(code, body, out.status, out.http_code, out.error, sizeof(out.error));
    if (out.status != Status::Ok) return;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        out.status = Status::ParseError;
        snprintf(out.error, sizeof(out.error), "bad pickjobs json");
        return;
    }

    for (JsonObjectConst j : doc["jobs"].as<JsonArrayConst>()) {
        if (out.n_jobs >= PickJobsResult::MAX_JOBS) break;
        PickJobInfo& job = out.jobs[out.n_jobs++];
        snprintf(job.id,         sizeof(job.id),         "%s", j["id"]           | "");
        snprintf(job.name,       sizeof(job.name),       "%s", j["name"]         | "");
        snprintf(job.requested,  sizeof(job.requested),  "%s", j["requested_at"] | "");
        snprintf(job.job_status, sizeof(job.job_status), "%s", j["status"]       | "");
        job.n_items = 0;
        for (JsonObjectConst it : j["items"].as<JsonArrayConst>()) {
            if (job.n_items >= (int)(sizeof(job.items) / sizeof(job.items[0]))) break;
            PickJobItem& item = job.items[job.n_items++];
            snprintf(item.part_id,   sizeof(item.part_id),   "%s", it["part_id"]   | "");
            snprintf(item.part_name, sizeof(item.part_name), "%s", it["part_name"] | "");
            item.qty    = it["qty"]    | 0;
            item.picked = it["picked"] | false;
        }
    }
    capture_success();
}

// ---- POST /pickjobs/{id}/items/{idx}/pick ------------------------------

JobPickResult pick_job_item(const char* job_id, int item_idx,
                            int slot_num, const char* op_id) {
    JobPickResult r{};
    r.status = Status::NotConfigured;
    if (!job_id || !job_id[0]) {
        snprintf(r.error, sizeof(r.error), "empty job id");
        return r;
    }
    if (wifi_mgr::status() != wifi_mgr::State::Connected) {
        r.status = Status::NoWifi;
        snprintf(r.error, sizeof(r.error), "wifi not connected");
        return r;
    }
    if (url_error(r.error, sizeof(r.error))) return r;

    char tail[64];
    snprintf(tail, sizeof(tail), "pickjobs/%s/items/%d/pick", job_id, item_idx);
    char url[200];
    if (!build_url(url, sizeof(url), tail)) {
        snprintf(r.error, sizeof(r.error), "URL too long");
        return r;
    }

    char body_buf[96];
    {
        JsonDocument req;
        req["slot_num"] = slot_num;
        req["op_id"]    = op_id ? op_id : "";
        serializeJson(req, body_buf, sizeof(body_buf));
    }

    String body;
    int code = do_request("POST", url, body_buf, body);
    classify(code, body, r.status, r.http_code, r.error, sizeof(r.error));
    if (r.status != Status::Ok) return r;

    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
        r.item_picked = doc["item"]["picked"] | false;
        snprintf(r.job_status, sizeof(r.job_status), "%s", doc["job_status"] | "");
    }
    capture_success();
    return r;
}

} // namespace inv_api
