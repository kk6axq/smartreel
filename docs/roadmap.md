# SmartReel — roadmap

What's done, what's next, in what order, and why. The point of this
doc is to keep us from (a) building everything at once and (b)
building things that have to be torn out in three months.

Update this when phase exits land or when something changes the plan.

---

## Where we are today (2026-05-18)

**ESP32-S3 HMI** (Waveshare 4.3B board, arduino-esp32 3.2.1 / IDF 5.3
via pioarduino):
- Hardware: 800x480 RGB LCD with bounce-buffer + double FB, GT911
  touch, CH422G I/O expander, SDMMC 1-bit, RS485 UART1 half-duplex
  via SP3485.
- LVGL 8.4 with 11 cached screens, persistent status bar, anomaly +
  WiFi-password modals.
- WiFi: scan + connect from the Network screen, credentials persisted
  to `/sdcard/config.json`, status pill in topbar.
- SD card: auto-format on mount fail, explicit Format SD button, JSON
  config store (config + rack), atomic writes.
- RS485: full master per `docs/smartreel-rs485-protocol.md`. Sync
  request/response with retry, background POLL @ 20 ms, async events
  dispatched to a registered handler, FW update wire constants
  defined but state machine not yet built.
- Known issue: `docs/known-bugs.md` -> "screen tearing after WiFi
  scan populates SSID list".

**Core PCB** (RP2040/RP2350): Rev 1 PCBs ordered, no firmware yet.

**Not built yet** on the ESP32 side: RS485-event-to-UI plumbing,
SD-backed runtime state (vs. config), Inventree client, OTA web
admin, RP2040 FW push.

---

## Architectural decisions that must happen before we build more

These set what gets written and where; getting them wrong = rework.

### A1. Should we offload WiFi/HTTP to a Pico W?

**Proposal**: replace the RP2040 on the Core PCB with a Pico W
(RP2040 + CYW43439 WiFi). The ESP32-S3 keeps the HMI + SD + RS485
master role; the Pico W picks up WiFi, the Inventree HTTPS client,
the OTA web server, and possibly mDNS / time sync. The two boards
talk only over RS485.

**What this buys us**
- UI thread on ESP32 is genuinely isolated from network jitter. The
  LVGL refresh budget never competes with a TLS handshake or a slow
  Inventree response. We avoid having to be careful about *which*
  task does HTTPS.
- Security posture: the network-facing MCU is the smaller, simpler
  one. The HMI MCU sits behind RS485 and is unreachable from the
  WAN even if Inventree credentials leak.
- Crash isolation: a misbehaving WiFi stack can't lock the
  touchscreen.

**What it costs**
- Schematic + PCB respin (we're on Rev 1 -> Rev 2 anyway eventually,
  but this is a real cost). Pico W is pin-compatible with Pico for
  most things but not all.
- The RS485 protocol grows two new categories ("HTTP fetch" and
  "FW image transport"), with all of an Inventree pick-list now
  flowing as protocol messages instead of as `HTTPClient.get()`.
  Plausible volume: a pick-list response is ~5-20 KB JSON; chunked
  over 921600 baud = a few hundred ms. OK but not free.
- WiFi credentials still need to live somewhere. Either we store
  them on the SD card on the ESP32 side and push them to the Pico
  on every boot, or the Pico has its own flash partition for
  network state and the HMI's Network screen RPCs over to read /
  write. The first is simpler.
- The OTA flow gets ugly: a user uploads an ESP32 firmware image to
  a web page served from the Pico, and those bytes flow over RS485
  to the ESP32 to be written to its own flash. ~14 MB OTA budget
  @ 921600 baud = ~150 s, workable but adds a new failure mode.
- Two firmwares, two WiFi codebases, two OTA flows.

**Reality check**
The ESP32-S3 is dual-core and we already have LVGL on APP_CPU and
WiFi on PRO_CPU. The actual UI-blocking issues we've hit are
fixable at the LVGL layer (see the tearing entry in known-bugs).
HTTPS handshakes are 200-500 ms but only need to happen rarely
(on Inventree poll, ~once per minute, or on user-initiated action).
A worker task pattern handles this cleanly.

**Recommendation: stay on ESP32-S3 for WiFi/HTTP.** Move to the
Pico W route only if we hit a concrete problem with one of:
  (a) UI judder we can't fix in LVGL,
  (b) a product requirement for network isolation,
  (c) Inventree calls actually taking too long to fit in a worker
      task budget.

Revisit this decision at the end of Phase 2 (Inventree client). If
it works there, we're done with this question.

### A2. Persistent state model

We currently have **config** on SD (`config.json`, `rack.json`).
We need **state** too — things that change at runtime and must
survive a reboot.

**Two-file split** (write each independently, atomic via tmp-file
rename, already implemented in `sdcard::write_file_atomic`):

```
/sdcard/config.json   -- WiFi, Inventree URL/token, display prefs    (rarely changes)
/sdcard/rack.json     -- chain count, slot numbering, dividers       (rarely changes)
/sdcard/state.json    -- per-slot occupancy + part assignments       (changes often)
/sdcard/jobs.json     -- pick queue + per-job progress               (changes often)
/sdcard/anomalies.jsonl -- append-only event log, capped + rotated   (changes often)
```

Why split: keeps high-write files away from the read-only-at-boot
config. Anomalies as JSONL means we never rewrite the whole file
on each event; we append a line and trim by line count once it gets
big. JSONL is also easy to grep when reviewing on a host machine.

**Where the truth lives**: SD is canonical. `app_state` is the
in-memory mirror, rebuilt from SD on boot. UI actions:
1. Update `app_state` immediately so the UI responds.
2. Enqueue a write to the SD writer task.
3. Writer task batches writes (~250 ms debounce) so a burst of
   slot updates doesn't thrash the SD card.

This is a real change from today, where the state IS the mock data
in `app_state.cpp`. The mocks stay around as the "no SD card"
fallback.

### A3. Threading model — UI responsiveness invariants

Cross-cutting rule for every feature below:

```
The LVGL task NEVER blocks on:
  - SD writes (use the writer task; reads are OK at <10 ms each)
  - Network I/O (TLS handshake, HTTP req/resp -> Inventree worker)
  - RS485 transactions that aren't already cached state
  - JSON serialise of anything larger than ~1 KB
```

If a UI action needs an answer from a slow source, the handler:
- Returns immediately with the cached/optimistic value
- Enqueues the work
- Re-renders the affected widget when the worker signals back
  (LVGL `lv_async_call` is the bridge — it queues a callback that
  runs on the LVGL task on the next refresh)

Tasks today:
- `lvgl` task -- pinned APP_CPU, priority 2
- `rs485-poll` task -- pinned PRO_CPU, priority 1
- WiFi internal tasks -- PRO_CPU, owned by the radio driver

Tasks we'll add:
- `sd-writer` task -- priority 1, runs anywhere; receives write
  requests from a queue, debounces, calls `sdcard::write_file_atomic`.
- `inventree` worker -- priority 1, runs on PRO_CPU; processes a
  queue of HTTP requests; signals results via `lv_async_call`.
- `http-admin` task -- only when the admin web server is on; lwIP
  callbacks run from the WiFi task, but heavy work (OTA write,
  firmware push) happens on this task.

---

## Phase 1 — SD-backed state + RS485-to-UI plumbing

**Goal**: every action a user can take in the UI changes real state
and is reflected on the LEDs of a real reel; every event from a
real reel updates the UI and the SD.

**Deliverables**
1. `state_store` module mirroring the `config_store` pattern but for
   `state.json` + `jobs.json` + `anomalies.jsonl`. SD writer task.
2. `app_state` becomes the in-memory view of those files (today it
   has the structure but is hard-coded with mock data).
3. RS485 event handler in `main.cpp` is replaced with a real
   dispatcher:
   - `EVT_INPUT_CHANGE` -> update slot occupancy, advance pick job
     if one is active, write to SD, raise anomaly modal if the
     change wasn't expected.
   - `EVT_REEL_INSERTED` / `EVT_REEL_REMOVED` -> update rack chain
     composition, write to SD, raise anomaly if not in Load /
     Maintenance mode.
   - `EVT_SENSE_THRESHOLD` -> log only for now.
   - `EVT_LOG` -> ship to Serial.
4. UI actions wired to RS485 commands:
   - "Light target slots" during a pick job -> `set_reel_pixels` +
     `commit_pixels`.
   - "Find part" on Inventory screen -> light the LED briefly.
   - Self Test "All LEDs" / "Single slot" buttons -> actually fire.
5. Cross-thread dispatch helper: `app::dispatch_on_lvgl(fn, arg)` =
   `lv_async_call`. Used by RS485 event handler and SD reads.

**Out of scope for Phase 1**
- Inventree calls. Pick jobs come from the mock or from a manually
  scanned QR. (Future Phase 2 wires them to a real Inventree
  pick-list pull.)
- WiFi work. WiFi can be off and the system still works.
- OTA. We flash via USB.

**Exit criteria**
- Power-cycle the unit -> the rack screen shows the same slot
  occupancy it had before power-off.
- Yank a reel without an active job -> anomaly modal appears AND
  the slot turns red on the dot grid AND the anomaly is in
  `anomalies.jsonl`.
- Start a mock pick job from the Queue screen -> Core PCB lights
  the target slots; advancing each pick updates the progress bar
  and writes to `jobs.json`; cancelling the job clears LEDs.

**Estimated effort**: ~3 days.

---

## Phase 2 — Inventree client

**Goal**: pick jobs come from a real Inventree server; inventory
counts update Inventree when reels are consumed.

**Deliverables**
1. `net/http_client` module that wraps `NetworkClientSecure` with a
   request queue. Public API is:
   ```
   net::http::get_json(url, headers, cb)
   net::http::post_json(url, body, headers, cb)
   ```
   `cb` runs on the inventree worker thread; caller dispatches the
   UI update via `lv_async_call`.
2. `net/inventree` module: a thin Inventree v0.16 API client.
   Minimum endpoints to start:
   - `GET /api/order/po/?status__in=10,15` -> open pick lists
   - `GET /api/order/po/<id>/lines/` -> per-list items
   - `PATCH /api/order/po/<id>/lines/<n>/` -> mark line picked
3. Persistent cache on SD: when we fetch a pick list we drop it to
   `/sdcard/cache/po-<id>.json` so the UI can render it instantly
   on next entry even if WiFi is down.
4. Queue screen pulls live data via the worker; falls back to cached
   `/sdcard/jobs.json` if offline. Connection-status pill in the
   topbar gets a tooltip with last-sync-time.
5. TLS root cert handling: bundle the Let's Encrypt R3 + ISRG X1
   root in `include/net_certs.h` for the common case; allow user to
   drop a custom `.pem` on the SD card for self-hosted Inventree
   with a private CA.

**Out of scope for Phase 2**
- mDNS / Bonjour. Use IP or hostname from config.
- Inventree write paths beyond marking lines picked.
- Multi-user / auth flows beyond the static API token.

**Exit criteria**
- WiFi off / Inventree down -> Queue screen still renders cached
  jobs; topbar pill says "OFFLINE", last-sync timestamp visible.
- WiFi on -> Queue screen shows live jobs within ~2 s of entry.
- Marking a line picked propagates to Inventree within ~5 s
  (visible at https://inv.lab.local/api/order/po/<id>/lines/<n>).
- UI refresh rate doesn't dip during an HTTPS round trip
  (measured with `lv_perf_monitor` enabled in dev builds).

**Estimated effort**: ~4-5 days. TLS is the biggest unknown — first
session handshake on the ESP32-S3 takes ~400 ms; cached sessions
~100 ms. If we're talking to Inventree more than once a minute we
should keep the connection alive.

**Pico W gate**: if Inventree calls visibly hitch the UI even with
the worker pattern, *now* is the time to revisit decision A1.

---

## Phase 3 — Admin web interface + OTA

**Goal**: a tiny HTTP server on the ESP32 lets a workshop laptop
upload a new HMI firmware (`.bin`), trigger a Core firmware push,
view a status page, and download the log buffer.

**Deliverables**
1. `net/http_admin` module: lwIP `httpd` (or `esp_http_server`)
   with three routes:
   - `GET /` -> single-page status: WiFi, Inventree status, RS485
     stats, recent anomalies, FW versions.
   - `POST /ota/hmi` -> multipart upload of an `.bin` for the ESP32
     itself; writes via `esp_https_ota`-style API or raw
     `esp_ota_*`, verifies, reboots.
   - `POST /ota/core` -> multipart upload of the RP2040 image;
     spools the bytes through the RS485 firmware update flow
     (`MSG_FW_BEGIN/CHUNK/VERIFY/COMMIT/CONFIRM` already defined
     in `rs485_proto.h`).
2. `rs485` FW-update state machine on the ESP32 side: `fw_update`
   submodule that calls `poll_pause()`, runs the protocol, calls
   `poll_resume()` on completion. Reports progress via a callback
   so the web page can show a progress bar.
3. Admin auth: HTTP basic auth against a token stored in
   `config.json`. The same token used for Inventree token reveal in
   the UI.
4. Optional: a `GET /log` endpoint that returns the last N entries
   from a ring buffer in PSRAM (we don't have one yet — add it as
   part of Phase 3).

**Out of scope for Phase 3**
- HTTPS for the admin interface. Workshop network is trusted; we
  redirect HTTPS attempts to HTTP with a notice. (Easy to add
  later with the same cert handling Phase 2 introduces.)
- Account management. Single shared admin token.
- WebSocket live-update of the status page. Polling at 2 s is fine.

**Exit criteria**
- Open the web page, see live status that matches what's on the
  touchscreen.
- Upload a new HMI binary -> device reboots into it -> status page
  shows the new version.
- Upload a new Core binary -> RS485 FW push runs -> reel LEDs come
  back on after the Core reboots into the new image.
- During a Core FW push, the HMI UI is still responsive (you can
  navigate screens, just can't issue new reel commands until done).

**Estimated effort**: ~4 days, of which the RP2040 FW update flow
is probably half. The web part is small once the worker pattern
from Phase 2 is in place.

---

## Phase 4 — Polish backlog

Order roughly by how much they bug us. Pick up between phases or
when the touchscreen is open and we notice.

- **Fix WiFi-scan list tearing** (`docs/known-bugs.md`). Try
  hypothesis #1 (preallocate row contexts) and #2 (drop "always
  dirty" for ConfigNetwork) first.
- **Build perf**: see if we can pre-build the LVGL static lib so
  edits to `lv_conf.h` don't trigger a full LVGL recompile. Look at
  ccache wrapping; check if pioarduino exposes a Ninja generator.
- **Drop the .pio-libdeps WiFi-WhenIdle drift**: arduino-esp32 v3
  occasionally regenerates `lib326/WiFi/*` on no apparent change.
  Audit lib_ldf_mode interactions.
- **Live RSSI / WiFi reconnect notification**: the Network screen
  reflects state on entry but doesn't update while open. Hook a
  WiFi event -> `lv_async_call(rebuild_current)`.
- **Anomaly log review screen**: a screen that reads
  `anomalies.jsonl` and shows the last 50 entries, with filter +
  acknowledge. (Today the Anomaly screen builder exists but is
  fed by mock data.)
- **i18n hooks**: just `tr("string")` macros, no actual second
  language yet. Lets the future EN/DE/JP swap not need a refactor.
- **NTP**: clock in the topbar mock is hard-coded "14:32". Once
  WiFi is up by default, plumb in IDF's SNTP.

---

## Explicit non-goals (for now)

Things that sound reasonable but would be premature. Revisit if a
real need shows up.

- **Bluetooth pairing / commissioning**: WiFi credentials over the
  touchscreen + SD-card override is sufficient.
- **mDNS / `reelrack.local`**: nice to have, not needed if the IP
  is on the status page.
- **MQTT**: Inventree API is REST; no broker required.
- **OPC-UA / Modbus TCP**: industrial protocols. Out of scope for
  a single-rack lab tool.
- **TLS for the admin web server**: cert provisioning is a real
  burden in a workshop network; HTTP is fine when behind WPA2.
- **Multi-rack / multi-HMI sync**: one rack, one HMI. Out of scope.
- **Cloud anything**: all state local. Inventree is the only
  network dep, and even that's optional.
- **Theming / dark mode**: the HMI mockup is one theme. Designers
  can iterate the CSS in the mockup; we re-port tokens to
  `theme.cpp` when the design settles.
- **Animations on the LCD**: cheap to add later if we want; for
  now flat redraws keep the LVGL refresh budget predictable.

---

## Open questions / unknowns

- **TLS handshake budget on the ESP32-S3 with PSRAM-backed mbedTLS
  arena**: not yet measured. Could move the needle on decision A1.
- **Core firmware language and SDK**: C with Pico SDK, MicroPython,
  or Rust embassy? Affects how easy the FW update bootloader is
  to write.
- **Reel ID scheme**: sense resistor only, or sense + 1-Wire EEPROM?
  Resolves how `EVT_REEL_INSERTED` payload is structured.
