# SmartReel — InvenTree Integration

How the SmartReel plugin lives inside InvenTree, how to install and configure
it, and the sync model the HMI uses to stay consistent with InvenTree.

## 1. How the plugin works

The plugin (`inventree-plugin/smart_reel/`, slug `smartreel`, v0.2.0) is a
standard InvenTree plugin that exposes a **workflow-shaped HTTP API** the HMI
calls, plus two web-UI panels. It deliberately does **not** proxy InvenTree's
generic REST API; instead it offers exactly the operations the HMI needs
(resolve / register / assign / pick / clear / pickjobs / locate / anomaly /
provision) and translates them into InvenTree model operations. That keeps the
HMI simple.

Layered source:

```
core.py     plugin class — settings, URL routes, UI panels
api.py      DRF views — HTTP layer, op_id idempotency, auth + error mapping
services.py domain logic — StockItem.move(), location/metadata management
static/panel.js  the two browser panels (no build step)
```

Mixins used: `UrlsMixin`, `SettingsMixin`, `UserInterfaceMixin`. Barcode
resolution rides on InvenTree's own barcode plugin registry, so any code
InvenTree can resolve (StockItem QR labels, linked custom codes) resolves here.

### Data model mapping

| SmartReel concept | InvenTree representation |
|---|---|
| A rack | a `StockLocation` tagged `metadata['smartreel']={"is_rack":true, ...}` |
| A slot | a child `StockLocation` of the rack, tagged `{"slot": n}`, named "Slot NN" |
| A reel in a slot | a `StockItem` whose `location` is that slot sub-location |
| A pick job | data stored on a `Build`'s `metadata['smartreel']` |
| An anomaly | an entry in the rack location's `metadata['smartreel_anomalies']` (cap 200) |

All stock movement uses `StockItem.move(destination, reason, user)`, which
creates a `StockHistory` audit entry. **Picks are whole-reel transfers** — the
entire StockItem moves; there is no quantity decrement model.

## 2. Installation and configuration

> Plugin installation follows InvenTree's normal plugin process; confirm
> specifics against the InvenTree version you run.

1. **Install the plugin** into the InvenTree instance (the package's entry point
   is `SmartReelPlugin = "smart_reel.core:SmartReelPlugin"`). Enable it in
   InvenTree's plugin settings.
2. **Enable plugin URLs and the plugin interface** in InvenTree's global
   settings (`ENABLE_PLUGINS_URL`, `ENABLE_PLUGINS_INTERFACE`) so the
   `/plugin/smartreel/...` routes and the web panels are served. Re-run
   InvenTree's frontend asset build (`invoke static`) if panel JS is stale.
3. **Configure plugin settings** (InvenTree → Settings → Plugins → Smart Reel):

   | Setting | Meaning |
   |---|---|
   | Staging location | default destination for picked reels (`STAGING_LOCATION`) |
   | Pulled location | destination for anomaly `/clear` removals (`PULLED_LOCATION`) |
   | HMI URL (optional) | explicit public base URL embedded in provisioning QRs; otherwise InvenTree's base URL or the request host is used |

   There is **no global rack-location setting** — racks are provisioned
   individually (next step). Staging/Pulled are instance-wide defaults that a
   rack can override in its own metadata.

4. **Provision each rack.** Open the `StockLocation` that represents the rack →
   the **SmartReel HMI** panel → *Provision*. The plugin tags the location as a
   rack, mints an API token bound to it, and shows the setup QR. Scan it on the
   HMI (Settings → Network → *Scan setup code*). Use *Rotate token* to revoke
   and reissue.

### Dev workflow (for plugin developers)

The plugin ships dev helpers (under `inventree-plugin/`):

- `dev-restart.sh` — restart the docker containers + health check.
- `test-live.sh` / `test_live.py` — ~32 contract checks against a running docker
  instance.
- `test-multirack.sh` / `test_multirack.py` — multi-rack isolation tests.
- `set-hmi-url.sh` — set the HMI-URL setting.

> The docker InvenTree dev instance serves plain HTTP via Caddy on
> `inventree.localhost`. The HMI is **HTTPS-only**, so for HMI↔real-InvenTree
> testing either enable HTTPS in the Caddyfile (with a LAN-IP SAN) or test
> against the mock (already TLS).

## 3. The sync model: op queue + reconcile

The HMI's `app/inv_sync` module owns *all* InvenTree traffic and keeps the rack
consistent through two mechanisms.

### 3.1 The op queue (writes)

Every inventory-changing action becomes an **op** on a 16-deep queue:
`Assign`, `Pick`, `JobPick`, `Clear`, `Anomaly`. Each op carries a unique
`op_id` and is retried (up to 20 attempts) until it succeeds:

- A network/5xx failure → retry.
- A `4xx` → permanent reject (logged); the next reconcile catches any resulting
  drift.

Because the server caches `op_id → response` for 10 minutes and replays it,
retries are safe and can never double-apply a move. This is what lets the HMI
keep working through a flaky link without losing or duplicating inventory
changes.

### 3.2 Reconcile (reads)

The HMI fetches a full snapshot (`GET /rack`) at boot, every ~60 s, and on
demand, then compares **server truth** (the snapshot) against **physical truth**
(the sensors via the Core's hardware mirror):

| Server | Physical | HMI action |
|---|---|---|
| empty (was occupied) | present | light slot amber + "Stock moved in InvenTree" prompt; removing the reel resolves it (no inventory call) |
| occupied | absent | log anomaly + `clear` the slot to the Pulled location |
| occupied | present | adopt the server's part/qty for that slot (boot sync) |

Reconcile skips slots the operator is actively working (a lit pick TARGET, a
just-PICKED slot) and skips entirely when the Core hardware mirror is invalid
(Core absent).

## 4. Online / offline behaviour

- The HMI health-checks the server every **15 s** while online and **5 s** while
  offline; "online" means the last good health check is younger than **45 s**.
- **Picks are blocked offline** (a banner explains why), so the rack can never
  report a removal InvenTree doesn't know about.
- **Put-away** degrades gracefully: assigns queue for later; loads against the
  on-SD offline catalog stay local until reconciled.
- The HMI is **HTTPS-only**; it refuses to send the token over cleartext.

## 5. Multi-rack support

One InvenTree instance can drive many independent racks. The design is
token-bound rack identity:

- Each rack is its own `StockLocation`; provisioning binds the issued API token
  to it (`token.metadata['smartreel_rack'] = <loc_pk>`).
- Every rack-scoped request resolves its rack from `request.auth`. An **unbound
  token is refused with 409** (re-provision).
- HMIs need no rack number and no wire/protocol change — the token alone tells
  the plugin which rack it is driving.
- Pick jobs are created from selected reels: the Build Order panel lists the
  candidate reels (`GET /pickjobs/options`), and `POST /pickjobs/from-build`
  takes `stock_ids` and fans out **one job per rack** that holds a selected reel.
  Each item is pinned to its `stock_id`. `GET /pickjobs?all=1` and `GET /racks`
  serve the web panel across all racks.

## 6. The mock server

`debug-fw/mock-inventree/` is a FastAPI service implementing the same `/api/v1/*`
contract so the HMI can be developed without a live InvenTree.

- Start: `./setup.sh` (one-off: venv + deps + cert), then `./run.sh` →
  `https://0.0.0.0:8443`, token `dev-token`. `SR_HTTPS=0 ./run.sh` for plain
  HTTP.
- Self-signed cert under `certs/` (generated by `gen-cert.sh`, with LAN-IP SANs
  and a printed SHA-256 fingerprint for HMI pinning).
- Same `op_id` idempotency (cache TTL `SR_OP_CACHE_TTL_S`, default 600 s).
- Provisioning: `GET /_dev/provision` returns the `SRPROV1:` payload + a QR SVG
  (via `segno`). The dashboard renders the QR so bench provisioning works the
  same way as the real plugin's panel.
- A dashboard (polls every 2 s) shows the rack grid, jobs, anomalies, and op
  cache. Dev helpers: `/_dev/reset`, `/_dev/anomalies`, `/_dev/ops`,
  `/_dev/inject_error` (to test failure UI).
- `smoke-test.sh` exercises the whole contract on a throwaway port.
- **Single-rack only** — multi-rack lives in the real plugin. The mock uses a
  fixed rack location id (`SR_RACK_LOCATION_ID`, default 42).

Key env vars: `SR_TOKEN`, `SR_HTTPS`, `SR_PORT`, `SR_HOST`,
`SR_RACK_LOCATION_ID`, `SR_STAGING_LOCATION_ID`, `SR_PULLED_LOCATION_ID`,
`SR_N_CHAINS`, `SR_SLOTS_PER_CHAIN`, `SR_LATENCY_MS`, `SR_OP_CACHE_TTL_S`.

## 7. Find / locate

`GET /parts/locate?part_id=<ipn-or-pk>` returns the slot numbers in the token's
rack currently holding the part — this powers the HMI's *Find* button
independent of pick jobs.

> A plugin "locate" feature was being refined alongside this documentation. The
> `/parts/locate` query is the current shape; if a dedicated locate mixin lands,
> the find flow may gain richer behaviour (e.g. driving the rack LEDs from the
> InvenTree side). Treat this section as current-state, possibly evolving.
</content>
