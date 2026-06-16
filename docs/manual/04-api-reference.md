# SmartReel — API Reference

Two interfaces are documented here:

- **Part A — the HMI ↔ plugin HTTP API** (`/api/v1/*` over HTTPS), implemented
  identically by the InvenTree plugin and the development mock.
- **Part B — the RS485 wire protocol** between the HMI and the Core firmware.

This builds on `docs/hmi-plugin-api.md` and `docs/smartreel-rs485-protocol.md`
and is reconciled against the code. Where the code differs from the older docs
it is flagged.

---

## Part A — HMI ↔ plugin HTTP API

### A.1 Base URL and routing

- Real plugin: `https://<inventree-host>/plugin/smartreel`
- Mock: `https://<dev-host>:8443`

The HMI appends `/api/v1/<path>` to its configured base URL, so both servers
are reached identically.

### A.2 Conventions

- **Auth** — every endpoint except `GET /health` requires
  `Authorization: Token <token>`. The real plugin accepts InvenTree API tokens;
  the mock compares against `SR_TOKEN` (default `dev-token`).
- **HTTPS only** — the HMI refuses to send the token over cleartext; non-HTTPS
  base URLs are rejected client-side.
- **Idempotency** — every mutating request carries an `op_id` string,
  `hmi-<chip-id>-<millis>-<n>`. The server caches `op_id → response` for
  **10 minutes** and replays the cached response on retry. The HMI's op queue
  retries until success precisely because this makes retries safe.
- **Slot numbers are physical positions** (1..N). Each maps to an InvenTree
  structural sub-location of the rack location, so a part shows as
  e.g. "SmartReel Rack A / Slot 32".
- **Rack identity rides on the token** — see [A.5](#a5-rack-identity--multi-rack).
- **Errors** — non-2xx with `{"detail": "<human-readable>"}`. Status mapping in
  the plugin: `404` not found, `409` conflict (slot occupied / logically empty /
  unbound token), `400` bad request, `403` not staff (provision).

### A.3 Endpoints

#### `GET /health` — liveness (no auth)
```jsonc
{ "ok": true, "server": "smartreel-plugin", "version": "0.2.0", "time": "..." }
```
Marked auth-exempt server-side. The HMI polls this to set its online/offline
state.

#### `POST /rack/register` — boot-time self-registration
```jsonc
// req
{ "n_slots": 64, "op_id": "..." }
// resp
{ "location_id": 42, "slot_locations": [ { "slot": 1, "location_id": 4201 }, ... ] }
```
Idempotent. Ensures the rack location and one sub-location per physical slot
("Slot 01"…"Slot NN", tagged `metadata['smartreel']={'slot':n}`); creates
missing ones if `n_slots` grew. Rack is resolved from the token.

#### `GET /rack` — full rack snapshot
Used at boot and for periodic reconciliation. Doubles as the reconcile source
(detecting stock moved in InvenTree without a physical removal).
```jsonc
{
  "location_id": 42,
  "n_slots": 64,
  "slots": [
    {
      "slot": 3,
      "location_id": 4203,
      "stock": {                  // null when logically empty
        "id": 1001,
        "qty": 5000,
        "batch": "B-241001",
        "barcode": "MOCK-PART-00001",
        "part": { "id": "R-10K-0805", "name": "RES 10kohm 1% 0805",
                  "pkg": "0805", "mfg": "Yageo" }
      }
    }, ...
  ],
  "pickjobs_available": 2
}
```

#### `GET /rack/occupancy` — cheap occupancy fingerprint
```jsonc
{ "rev": "9f2a1c0b4d7e6f81" }
```
An opaque hash over the rack's slot occupancy (+ pending locates). The HMI polls
this every ~5s and runs the full `GET /rack` reconcile only when `rev` changes,
so a reel moved out of a slot in InvenTree is detected within ~10s without a full
snapshot each poll (item 6). Compare for equality only; the value is not stable
across server versions.

#### `POST /barcode/resolve` — what did I just scan?
```jsonc
// req
{ "code": "<raw scanned string>", "op_id": "..." }
// resp
{
  "type": "stockitem" | "part" | "location" | "unknown",
  "stock": { ...same shape as rack snapshot stock, plus "slot_num": 3|0 ... },
  "part":  { ... },              // when type == "part"
  "message": "Unknown barcode"   // when type == "unknown"
}
```
The real plugin resolves through InvenTree's barcode plugin registry, so any QR
InvenTree can print (including StockItem labels) resolves. `stock.slot_num` is
non-zero when the item is already in a rack slot. Rack is optional here (only
used to fill `slot_num`).

#### `POST /rack/slots/{n}/assign` — load a reel into a slot
```jsonc
// req
{ "stock_item_id": 1001, "op_id": "..." }
// resp
{ "slot": 3, "stock": { ... } }
// errors: 404 unknown slot/stock, 409 slot occupied by a different item
```
Transfers the StockItem into the slot's sub-location (audit trail via
`StockItem.move()`). If the item was in another rack slot, that slot is
implicitly freed.

#### `POST /rack/slots/{n}/pick` — single pick (whole reel)
```jsonc
// req
{ "op_id": "...", "destination_id": null }   // destination optional
// resp
{ "slot": 3, "stock_id": 1001, "moved_to": 99 }
// errors: 404 unknown slot, 409 slot logically empty
```
**Whole-reel semantics**: the StockItem moves to `destination_id` if given,
else the configured **Staging** location, and the slot becomes logically empty.
No quantity math.

#### `POST /rack/slots/{n}/clear` — reconcile a manual removal
```jsonc
// req
{ "op_id": "...", "reason": "anomaly:removed" }
// resp
{ "slot": 3, "stock_id": 1001, "moved_to": 98 }
```
Moves the slot's stock to the configured **Pulled** location. Returns 2xx even
if the slot was already empty (idempotent goal state).

#### `GET /pickjobs` — list pick jobs
```jsonc
{
  "jobs": [
    {
      "id": "BO-0042",
      "name": "PCB-Rev-A x 5",
      "requested_at": "2026-06-11T14:02:00Z",
      "status": "pending" | "partial" | "done",
      "destination_id": 99,
      "items": [
        {
          "idx": 0,
          "part_id": "R-10K-0805",
          "part_name": "RES 10kohm 1% 0805",
          "qty": 250,                  // required qty, informational on the HMI
          "picked": false,
          "located_slots": [3, 17]     // rack slots currently holding this part
        }, ...
      ]
    }, ...
  ]
}
```
- One **item** = one part. Removing a reel of that part from any lit slot picks
  the item (one reel per item).
- `status` is **derived server-side**: `pending` / `partial` / `done`.
- Without query args, returns only the requesting token's rack's jobs (HMI use).
  With `?all=1` (session auth) returns every rack's jobs (web panel).
- Items whose part isn't in the rack have `located_slots: []`.

#### `POST /pickjobs/{id}/items/{idx}/pick` — pick one job item
```jsonc
// req
{ "slot_num": 3, "op_id": "..." }
// resp
{ "item": { ...updated item... }, "job_status": "partial" | "done" }
// errors: 404 job/item/slot, 409 slot doesn't hold the item's part / already picked
```
Validates the slot holds the item's part, performs a whole-reel pick to the
job's destination, marks the item picked.

#### `GET /pickjobs/options?build_id={id}` — candidate reels for a build
```jsonc
{
  "lines": [
    {
      "part": 12, "part_id": "R-10K-0805", "part_name": "RES 10kohm 1% 0805",
      "qty": 250,
      "candidates": [
        { "stock_id": 1001, "qty": 5000, "batch": "A",
          "rack_id": 7, "rack_name": "Rack A", "slot_num": 3 }, ...
      ]
    }, ...
  ]
}
```
Web-panel endpoint (session auth). For each BOM line, every in-stock reel of
that part that lives in **any** SmartReel rack — what the panel's reel picker
offers (item 10a).

#### `POST /pickjobs/from-build` — create jobs from a Build Order
```jsonc
// req
{ "build_id": 42, "stock_ids": [1001, 1002], "destination_id": null, "op_id": "..." }
// resp
{ "jobs": [ { ...job object (rack A)... }, { ...job object (rack B)... } ] }
```
Web-panel endpoint (session auth). The selected reels are grouped by the rack
that holds them, creating **one job per rack** (item 10b); each job item is
pinned to its `stock_id`, so only that reel's slot lights. Replaces any prior
SmartReel jobs on the build.

#### `DELETE /pickjobs/{id}` — remove a build's jobs
```jsonc
{ "deleted": "BO-0042" }
```
Clears every rack's job for the build.

#### `GET /racks` — list configured racks (web panel)
```jsonc
{ "racks": [ { "location_id": 7, "name": "SmartReel Rack A", "n_slots": 64 }, ... ] }
```

#### `GET /parts/locate?part_id=R-10K-0805` — find a part
```jsonc
{ "part_id": "R-10K-0805", "slots": [3, 17] }
```
Powers "find this part" independent of pick jobs, scoped to the token's rack.
`part_id` may be an IPN or a pk string.

> The locate capability was being refined alongside this documentation (a
> possible "locate mixin" on the plugin). The `GET /parts/locate` query above is
> the current implementation; the area may evolve.

#### `POST /anomaly` — report sensor-detected anomalies
```jsonc
// req
{ "kind": "removed" | "added" | "divider", "slot_num": 12,
  "detail": "free text", "op_id": "..." }
// resp
{ "id": 7, "logged": true }
```
Audit log on the rack location's metadata (capped at 200 entries). Inventory
correction is separate (`/clear`).

#### `GET /provision?location=<pk>[&rotate=1]` — provision a rack (staff only)
Session-authenticated, staff only. Tags the location as a rack, mints/rotates an
API token bound to it, and returns the setup payload + QR:
```jsonc
{ "payload": "SRPROV1:{...}", "svg": "<svg>...", "token_name": "smartreel-rack-7",
  "base_url": "https://inventree.example/plugin/smartreel", "rack": { ... } }
```

### A.4 Provisioning payload (`SRPROV1:`)

Delivered as a QR so credentials don't have to be typed on the HMI:
```jsonc
SRPROV1:{"u":"https://inventree.example/plugin/smartreel",
         "t":"inv-token-...",
         "f":"AB:CD:..."}   // optional TLS cert SHA-256 fingerprint
```
- `SRPROV1:` prefix versions the payload and distinguishes it from a part
  barcode.
- `u` — plugin base URL (HMI appends `/api/v1/...`).
- `t` — InvenTree API token (or `dev-token` from the mock).
- `f` — optional cert fingerprint for pinning. **Parsed by the HMI but not yet
  enforced** (cert pinning is TODO; the HMI currently does not verify the TLS
  cert).

On the HMI: Settings → Network → *Scan setup code* → scan the screen. The HMI
stores URL + token and immediately runs `GET /health`.

### A.5 Rack identity & multi-rack

- Every rack is its own InvenTree `StockLocation` tagged
  `metadata['smartreel'] = {"is_rack": true, "staging"?, "pulled"?}`. Slots are
  child locations tagged `{"slot": n}`.
- **The token carries the rack.** Provisioning binds the issued API token to the
  location via `token.set_metadata('smartreel_rack', <loc_pk>)`. Every
  rack-scoped request resolves its rack from `request.auth`. An **unbound token
  is refused with 409** — re-provision. There is **no global rack setting** and
  no shared-rack fallback; every unit, including the first, is provisioned from
  its location page.
- `/health` and `/barcode/resolve` are rack-independent (resolve only uses the
  rack to fill `slot_num`).
- Pick jobs store a target rack pk; `GET /pickjobs` and `located_slots` are
  scoped to the requesting token's rack. `POST /pickjobs/from-build` takes
  `stock_ids` and fans out one job per rack. `GET /pickjobs?all=1` and `GET /racks` are for
  the web panel.

### A.6 Differences from `docs/hmi-plugin-api.md`

The code matches that contract closely. Notable points confirmed in code:

- Multi-rack is fully implemented (v0.2.0); the global `RACK_LOCATION` setting is
  gone — only `STAGING_LOCATION` and `PULLED_LOCATION` (instance-wide defaults,
  per-rack overridable in metadata) plus an optional HMI-URL setting remain.
- The op_id cache TTL is **600 s (10 minutes)**, matching the doc's "≥10
  minutes".
- The plugin issues the provisioning token for the staff user performing the
  provisioning (named `smartreel-rack-<pk>`); there is no separate fixed
  `smartreel-hmi` machine user baked into the plugin code.

---

## Part B — RS485 wire protocol

Between the HMI (bus master, address `0xFF`) and the Core (slave, `0x01`).
Constants below are from `esp32-hmi/src/rs485/rs485_proto.h` and
`core-fw/src/config.h`; both ends share the same frame codec.

### B.1 Frame format

```
+------+------+------+------+------+------+------+--------------+------+------+
| 0xAA | 0x55 | LEN_H| LEN_L| ADDR | SEQ  | TYPE | PAYLOAD ...  | CRC_H| CRC_L|
+------+------+------+------+------+------+------+--------------+------+------+
```

- **Sync** `0xAA 0x55` — receiver hunts for these to resync after a glitch.
- **LEN** — big-endian 16-bit; counts ADDR through end of PAYLOAD (minimum 3).
- **ADDR** — `0x00` broadcast, `0x01` Core, `0xFF` master (ESP32).
- **SEQ** — request/response matching; a response echoes the request's SEQ.
- **TYPE** — bit 7 set = response (`0x10` request → `0x90` response); bit 6 set
  = error response (`0xD0`). Low 6 bits are the command code.
- **CRC** — CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflect, no
  final XOR) over ADDR..end-of-PAYLOAD.
- **MAX_PAYLOAD** = 768 bytes; max frame 777 bytes.
- **Baud** = 115200, half-duplex.

### B.2 Message catalog (request codes)

#### System (0x0_)
| Code | Name | Dir | Notes |
|---|---|---|---|
| 0x00 | PING | E→C | echo |
| 0x01 | GET_VERSION | E→C | → major, minor, patch, build_id(4B), hw_rev |
| 0x02 | GET_STATUS | E→C | → uptime_s(4B), reels_present bitmap, error_flags(2B) |
| 0x03 | RESET | E→C | reason byte (0=normal, 1=enter_update_mode) |
| 0x04 | POLL | E→C | → "no events" or queued events (see B.4) |
| 0x05 | ACK_EVENTS | E→C | count + seq bytes; clears ACK'd events |

#### Reel I/O (0x1_)
| Code | Name | Dir | Notes |
|---|---|---|---|
| 0x10 | GET_REEL_INFO | E→C | reel_id (or 0xFF all) → per-port present, sense_mv, module_count |
| 0x11 | READ_INPUTS | E→C | reel_id → module_count + 4B input word per module |
| 0x12 | SET_POLL_RATE | E→C | reel_id, rate_hz(2B) |

`reel_id` is the **port** number (0–3). Each module's 32-bit input word is laid
out `D0 S0 D1 S1 … D15 S15` (D = divider, S = slot-present).

#### LED control (0x2_)
| Code | Name | Dir | Notes |
|---|---|---|---|
| 0x20 | SET_REEL_PIXELS | E→C | reel_id, start_idx, count, RGB… (stages to back buffer) |
| 0x21 | FILL_REEL | E→C | reel_id, R, G, B (stages + commits) |
| 0x22 | SET_BRIGHTNESS | E→C | reel_id (or 0xFF), brightness 0–255 |
| 0x23 | SET_ANIMATION | E→C | reel_id, anim_id, params (runs on the Core) |
| 0x24 | COMMIT | E→C | reel_bitmap — atomic `show()` across selected ports |

The stage-then-COMMIT pattern avoids visible tearing when updating multiple
ports in one frame.

#### Firmware update (0x3_)
| Code | Name | Dir | Notes |
|---|---|---|---|
| 0x30 | FW_BEGIN | E→C | total_size(4B), SHA-256(32B), version(4B) |
| 0x31 | FW_CHUNK | E→C | offset(4B) + data (512B); → bytes_received(4B) |
| 0x32 | FW_VERIFY | E→C | Core re-hashes the image; OK or hash-mismatch |
| 0x33 | FW_COMMIT | E→C | commit + self-reset |
| 0x34 | FW_CONFIRM | E→C | clears the "needs confirm" flag after a good boot |
| 0x35 | FW_BOOTED | C→E | event: new image booted |

> **Discrepancy with `docs/smartreel-rs485-protocol.md`.** That note places
> firmware messages in category `0xF_`. The **code uses `0x30`–`0x35`** because
> a `0xF_` code would have both the response (bit 7) and error (bit 6) bits set,
> making firmware requests indistinguishable from error responses on the wire.
> Trust the code: firmware messages are at `0x30`–`0x35`.

#### Async events (0x8_, Core → HMI, delivered in the POLL response)
| Code | Name | Payload |
|---|---|---|
| 0x80 | INPUT_CHANGE | port, module, prev(4B), now(4B), ts_ms(4B) |
| 0x81 | REEL_INSERTED | port, module_count, sense_mv(2B) |
| 0x82 | REEL_REMOVED | port, module_count |
| 0x83 | SENSE_THRESHOLD | port, sense_mv, threshold_id |
| 0x8F | LOG | level, ASCII message |

#### Error codes (first payload byte of an error response)
| Code | Meaning |
|---|---|
| 0x00 | OK |
| 0x01 | unknown command |
| 0x02 | bad payload |
| 0x03 | reel absent |
| 0x04 | busy (e.g. flash erasing) |
| 0x10 | FW hash mismatch |
| 0x11 | FW offset out of range |
| 0x12 | FW wrong state |

### B.3 Polling and half-duplex events

On a half-duplex bus the Core cannot transmit unsolicited. The master issues
`POLL` (0x04) every ~20 ms; the Core answers with up to N queued events. The
Core keeps a **32-slot event ring** with per-event sequence numbers and resends
un-ACK'd events on each poll until the master `ACK_EVENTS` (0x05) clears them.
This keeps button-event latency under ~50 ms while guaranteeing delivery up to
the ring's capacity.

POLL response payload: `count`, then for each event `type, seq, len, data…`.

### B.4 Core firmware update flow

| Step | Message | Notes |
|---|---|---|
| 1 | FW_BEGIN | size + SHA-256 + version; Core stages (erase can take seconds → BUSY) |
| 2 | FW_CHUNK ×N | 512-byte chunks; Core ACKs bytes received; master retries gaps |
| 3 | FW_VERIFY | Core re-hashes, compares to the announced SHA-256 |
| 4 | FW_COMMIT | Core writes metadata (pending slot, needs-confirm) and resets |
| 5 | (bootloader) | A/B bootloader boots the new slot |
| 6 | FW_BOOTED → FW_CONFIRM | new app announces itself; master confirms to lock it in |

If the new firmware never sends `FW_BOOTED`/never gets `FW_CONFIRM`, the
bootloader rolls back to the previous slot on the next reset.
</content>
