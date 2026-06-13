# SmartReel HMI ↔ InvenTree plugin API (v1)

Canonical contract between the SmartReel HMI and the SmartReel InvenTree
plugin. Two servers implement it:

- `inventree-plugin/` — the real thing, running inside InvenTree.
  Base URL: `https://<inventree-host>/plugin/smartreel`
- `mock-inventree/` — standalone FastAPI mock for bench development.
  Base URL: `https://<dev-host>:8443`

The HMI appends `/api/v1/<path>` to its configured base URL, so both
servers are reached identically. Behaviour is specified by
`docs/user-stories.md` — when this file and the user stories disagree,
the user stories win and this file is out of date.

## Conventions

- **Auth**: every endpoint except `GET /health` requires
  `Authorization: Token <token>`. The real plugin accepts InvenTree API
  tokens; the mock uses `SR_TOKEN` (default `dev-token`).
- **HTTPS only**: the HMI refuses to send the token over cleartext.
- **Idempotency**: every mutating request carries an `op_id` string
  (`hmi-<chip-id>-<millis>-<n>`). Servers cache `op_id → response` for
  ≥10 minutes and replay the cached response on retry.
- **Slot numbers are physical positions** (1..N). The HMI's logical
  slots (divider-combined) map to the anchor physical position of the
  reel. InvenTree models each physical slot as a structural
  sub-location of the rack location, so a part shows as e.g.
  "SmartReel Rack A / Slot 32".
- **Errors**: non-2xx with `{"detail": "<human-readable>"}`.

## Endpoints

### `GET /health` — liveness (no auth)

```jsonc
{ "ok": true, "server": "smartreel-plugin", "version": "0.2.0", "time": "..." }
```

### `POST /rack/register` — boot-time self-registration

```jsonc
// req
{ "n_slots": 64, "op_id": "..." }
// resp
{ "location_id": 42, "slot_locations": [ { "slot": 1, "location_id": 4201 }, ... ] }
```

Idempotent. Ensures the rack location and one sub-location per physical
slot exist; creates missing ones if `n_slots` grew.

### `GET /rack` — full rack snapshot

Used at boot to sync, and periodically for reconciliation (detecting
stock moved in InvenTree without a physical removal — user-stories.md
"Background: Inventree inventory locations").

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

The HMI owns physical truth (slot sensors); this snapshot is logical
truth. A slot that is logically empty but physically occupied means the
part was moved in InvenTree behind our back → the HMI lights that slot
and instructs the user to remove the reel (no inventory action on
removal — InvenTree already moved it).

### `POST /barcode/resolve` — what did I just scan?

```jsonc
// req
{ "code": "<raw scanned string>", "op_id": "..." }
// resp
{
  "type": "stockitem" | "part" | "location" | "unknown",
  "stock": { ...same shape as rack snapshot stock, plus "slot_num": 3|0... },
  "part":  { ... },              // when type == "part"
  "message": "Unknown barcode"   // when type == "unknown"
}
```

The real plugin resolves through InvenTree's barcode framework, so any
QR InvenTree can print (StockItem labels included) resolves here, as do
custom codes linked to a stock item at receival (matched by barcode
hash). In practice most scans are InvenTree-generated StockItem QR
labels — supplier codes are often 1D/Datamatrix, which the SmartReel
scanner doesn't read.
`stock.slot_num` is non-zero when that stock item is already in a rack
slot ("already in slot N" UX).

### `POST /rack/slots/{n}/assign` — load a reel into a slot

```jsonc
// req
{ "stock_item_id": 1001, "op_id": "..." }
// resp
{ "slot": 3, "stock": { ... } }
// errors: 404 unknown slot/stock, 409 slot occupied
```

Transfers the stock item into the slot's sub-location (with audit
trail). If the item was in another rack slot, that slot is implicitly
freed. Completes user story 1 after the HMI detects physical placement.

### `POST /rack/slots/{n}/pick` — single pick (whole reel)

```jsonc
// req
{ "op_id": "...", "destination_id": null }   // destination optional
// resp
{ "slot": 3, "stock_id": 1001, "moved_to": 99 }
// errors: 404 unknown slot, 409 slot logically empty
```

**Whole-reel semantics** (user story 3): transfers the slot's
StockItem to `destination_id` if given, else the plugin's configured
Staging location. The slot is then logically empty. No quantity math —
the reel physically left the rack.

### `POST /rack/slots/{n}/clear` — reconcile a manual removal

```jsonc
// req
{ "op_id": "...", "reason": "anomaly:removed" }
// resp
{ "slot": 3, "stock_id": 1001, "moved_to": 98 }
```

For anomaly paths (reel yanked without a scan/pick). Stock moves to the
configured "Unsorted / pulled" location so it isn't lost. 2xx even if
the slot was already logically empty (idempotent goal state).

### `GET /pickjobs` — list pick jobs

Jobs are created in InvenTree from build orders (plugin panel "Send to
SmartReel" on the Build Order page — user story 4).

```jsonc
{
  "jobs": [
    {
      "id": "BO-0042",
      "name": "PCB-Rev-A x 5",
      "requested_at": "2026-06-11T14:02:00Z",
      "status": "pending" | "partial" | "done",
      "destination_id": 99,            // staging unless overridden at send time
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

- An **item** is one part. The HMI lights `located_slots` for all
  unpicked items; removing a reel of that part from any lit slot picks
  the item (one reel per item).
- `status` is **derived server-side**: `pending` (nothing picked),
  `partial` (some picked), `done` (all picked). The HMI's cancel button
  is purely local — already-picked items stay picked and the job shows
  `partial`, resumable later (user story 4, partial-pick paragraph).
- Items whose part is not in the rack have `located_slots: []`; the HMI
  shows them as unfulfillable rather than hiding them.

### `POST /pickjobs/{id}/items/{idx}/pick` — pick one job item

```jsonc
// req
{ "slot_num": 3, "op_id": "..." }
// resp
{ "item": { ...updated item... }, "job_status": "partial" | "done" }
// errors: 404 job/item/slot, 409 slot doesn't hold the item's part,
//         409 item already picked
```

Validates the slot holds the item's part, performs a whole-reel pick to
the job's destination, marks the item picked.

### `POST /anomaly` — report sensor-detected anomalies

```jsonc
// req
{ "kind": "removed" | "added" | "divider",
  "slot_num": 12, "detail": "free text", "op_id": "..." }
// resp
{ "id": 7, "logged": true }
```

Audit log on the server side (the real plugin attaches a note/history
entry). Inventory correction is separate (`/clear`).

### `GET /parts/locate?part_id=R-10K-0805`

```jsonc
{ "part_id": "R-10K-0805", "slots": [3, 17] }
```

Powers "find this part" independent of pick jobs.

## Provisioning (how the HMI gets its URL + token)

Typing on the HMI is painful, so credentials are delivered by QR code
using the scanner that's already on the unit:

1. In InvenTree, the SmartReel plugin settings panel shows a **setup
   QR code** (and a button to regenerate it / revoke the old token).
   The token is an InvenTree API token issued for a dedicated machine
   user, so SmartReel access can be revoked independently.
2. On the HMI: Settings → Network → "Scan setup code" → scan the
   screen. The HMI parses the payload, stores URL + token in
   `config_store`, and immediately runs `GET /health` to confirm.

QR payload (one line of text):

```jsonc
SRPROV1:{"u":"https://inventree.example/plugin/smartreel",
         "t":"inv-token-...",
         "f":"AB:CD:..."}   // optional TLS cert SHA-256 fingerprint
```

- `SRPROV1:` prefix distinguishes a provisioning code from a part
  barcode (and versions the payload).
- `u` is the plugin base URL (HMI appends `/api/v1/...`).
- `f` is optional; when present the HMI may pin the server cert
  instead of blind-trusting it. Ignored by firmware that doesn't pin.

The mock prints an equivalent QR/payload from its dashboard so bench
provisioning works the same way.

## Plugin settings (real plugin)

| Setting | Meaning |
|---|---|
| Rack location | structural location representing this SmartReel unit |
| Staging location | default destination for picked reels |
| Pulled location | destination for anomaly `/clear` removals |

## Future work: multiple SmartReel units (NOT yet supported)

The plugin is currently **single-rack**: one global `RACK_LOCATION` /
`STAGING_LOCATION` / `PULLED_LOCATION`, and every endpoint resolves
through that one location. Many HMIs can point at the plugin, but they
all share the one rack. We need to support **multiple independent
SmartReel racks** on one InvenTree instance.

Planned approach (server-side only — no HMI or wire-protocol change,
since each unit already has its own URL + token):

- Each unit is its own rack `StockLocation`, tagged
  `metadata['smartreel'] = { is_rack, staging_loc, pulled_loc }`
  (staging/pulled per-rack, falling back to the global settings).
- **Bind the API token to its rack**: `ApiToken` is a `MetadataMixin`,
  so provisioning sets `token.set_metadata('smartreel_rack', <loc_pk>)`.
  Each request resolves its rack from `request.auth` — no rack id needed
  in the URL or from the HMI.
- Replace `services.rack_location()` with `rack_for(request)`; everything
  downstream (`slot_map`, snapshot, assign/pick/clear, register) keys off
  it. `/health` and `/barcode/resolve` stay rack-independent.
- Pick jobs store a target rack pk; `GET /pickjobs` and `located_slots`
  filter to the requesting token's rack. The Build Order "Send to
  SmartReel" panel gains a rack selector.
- Provisioning panel moves onto each rack location's page (already a
  location-page panel, so it scales naturally).

Open decision: staging/pulled shared across racks vs. per-rack
(leaning per-rack with a global fallback).

## Changelog

- **2026-06-13**: noted multi-unit support as future work (see above);
  plugin is single-rack for now.
- **2026-06-11**: rewritten against `docs/user-stories.md`. Picks are
  whole-reel transfers (qty decrement model removed); pick jobs come
  from build orders with derived `pending/partial/done` status; rack
  snapshot doubles as the reconciliation source; `destination_id`
  per job/pick.
