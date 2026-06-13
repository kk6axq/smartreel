# SmartReel — Session Notes 2026-06-12

Implemented the full user-story set (`docs/user-stories.md`) across the
InvenTree plugin, the mock server, and the HMI firmware. The user stories
doc is now the source of truth; the API contract was rewritten to match it
in **`docs/hmi-plugin-api.md`** (canonical for plugin + mock + HMI).

## Contract changes (docs/hmi-plugin-api.md)

- **Picks are whole-reel transfers.** `POST /rack/slots/{n}/pick` takes no
  qty; the StockItem moves to Staging (or `destination_id`) and the slot is
  freed. The qty-decrement / "Picked bin" model is gone.
- **Pick jobs come from build orders.** One item = one part; picking any
  reel of that part from a lit slot satisfies the item. Job status is
  derived (`pending`/`partial`/`done`); HMI cancel is local-only, jobs
  resume as `partial` (user story 4).
- **`GET /rack`** doubles as the reconciliation source ("stock moved in
  InvenTree without removal" → light slot + instruct removal).
- **Provisioning**: `SRPROV1:{"u":url,"t":token,"f":fingerprint?}` QR —
  scan instead of typing credentials on the HMI.

## InvenTree plugin (inventree-plugin/) — implemented + live-tested

- Full `/plugin/smartreel/api/v1/*` contract: health (auth-exempt via the
  middleware's `auth_exempt` view attr), rack snapshot, register (creates
  `Slot NN` sub-locations with `metadata['smartreel']={'slot':n}`),
  barcode resolve (InvenTree barcode-plugin registry → covers generated QR
  + linked codes), assign/pick/clear via `StockItem.move()` (audit trail),
  pickjobs in Build metadata, anomaly log on the rack location's metadata,
  provision endpoint (rotatable `smartreel-hmi` ApiToken + QR SVG).
- Settings: RACK_LOCATION / STAGING_LOCATION / PULLED_LOCATION
  (`model: stock.stocklocation` pickers).
- Web panels (`static/panel.js`): Build Order "Send to SmartReel" panel +
  provisioning-QR panel on the rack location page. Registered + serving
  (verified via API); **in-browser render still needs eyeballs** — Lukas
  will help test.
- `./test-live.sh` runs 32 contract checks against the docker instance —
  all green. `./dev-restart.sh` = restart containers + health check.
- Globals flipped on the dev instance: `ENABLE_PLUGINS_URL`,
  `ENABLE_PLUGINS_INTERFACE`; ran `invoke static` (frontend assets were
  stale → INVE-E1).
- NOTE: test seeding created locations (SmartReel Rack / Staging / Pulled /
  Receiving Inbox), parts (R-10K-0805, C-100N-0805, ASM-TEST-1), a test
  build order, and stock items in the dev instance; clean up manually if
  unwanted.

## mock-inventree — aligned

Same contract as the plugin (whole-reel picks, derived job status,
`/_dev/provision` payload+QR via segno). Dashboard updated (new slot
shape, provisioning card). `./smoke-test.sh` covers every endpoint.

## HMI firmware — all flows wired to InvenTree (build green, NOT flashed)

New module **`app/inv_sync`** (worker on APP_CPU prio 1) owns all traffic:
health poll (15 s ok / 5 s down) → `app_state.online`; rack register on
first contact; `GET /rack` reconcile every 60 s + on demand; pick-job
fetch on demand (throttled 10 s); a 16-deep retrying op queue
(assign/pick/job-pick/clear/anomaly) — op_id idempotency makes retries
safe; 4xx = permanent reject (logged, rack reconcile catches drift).

- **Load** (story 1): resolve keeps `stock_id`+qty; locked card shows qty;
  "already in slot N" surfaced; placement queues `assign`; offline SD-
  catalog loads stay local. Mock rng qty only used when offline.
- **Single pick** (story 3): View → Pick arms; physical removal queues the
  whole-reel pick → Staging. Pick disabled offline (banner explains).
- **Pick jobs** (story 4): list fetched from server on Pick screen entry;
  done jobs hidden; `partial` shows "partially picked" + Resume button.
  Hardware removal AND the manual "Picked" button report
  `pickjobs/{id}/items/{idx}/pick`. Cancel stays local (already-picked
  items remain picked server-side).
- **Reconcile** (locations background story): server-empty+physically-
  present+had-part → part cleared, slot WARN, amber LED, "Stock moved in
  InvenTree" modal; removal resolves it. Server-occupied+physically-absent
  → anomaly + clear-to-pulled. Both-occupied → adopt server part/qty
  (boot sync). Skipped when `hw_mirror` invalid (Core absent) or slot is
  TARGET/PICKED.
- **Anomalies**: tamper removal → `POST /anomaly` + `clear`; unscanned
  insert → `POST /anomaly` (added).
- **Provisioning**: Settings → Network → "Scan setup code" parses SRPROV1,
  saves url+token, auto-runs the connection test.
- **UI updates** (stories + memory todo): fonts DOUBLED vs the original
  build per Lukas — 12→24, 14→28, 16/20→36, tiles 24→48
  (`bump-fonts.sh`; status bar 64 px, rows 80 px, buttons 58 px min,
  inputs 54 px); dot grid moved off Home to a new **Rack** screen; Home
  is now 5 big tiles; **Configure renamed Settings**. Expect a clipping
  audit on first flash.

## Bench-test checklist (next session, with hardware)

1. Flash HMI; provision by QR from the mock dashboard or plugin panel.
2. Load flow against real InvenTree: scan an InvenTree StockItem QR label
   → place → stock item lands in the slot sub-location.
3. Single pick → reel transfers to Staging.
4. Send a pick job from a Build Order page → appears on HMI → pick →
   panel shows progress; partial + resume.
5. Yank a reel → anomaly logged server-side + stock in Pulled.
6. Move a stock item out of a slot in InvenTree → within 60 s the slot
   lights amber + modal instructs removal.
7. Font/layout audit — clipping in fixed-size widgets (status bar pills,
   text-entry modal, self-test cards).

## Watch / open questions

- HMI is HTTPS-only; the docker InvenTree serves plain HTTP via Caddy on
  `inventree.localhost`. For HMI-to-real-InvenTree testing, either enable
  HTTPS in the Caddyfile (LAN IP SAN) or test against the mock (already
  TLS). Cert pinning (`f` in SRPROV1) is parsed-but-unused.
- Job list refresh is skipped while a job is active (indices must not
  shift mid-pick); resumes on next Pick screen entry.
- `state_store` jobs.json doesn't persist the new `status` field (cache
  only; server refresh overwrites).
