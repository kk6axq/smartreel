# inventree-smart-reel

InvenTree plugin for the SmartReel smart reel holder.

Implements the HMI ↔ plugin contract from `docs/hmi-plugin-api.md` (repo
root) under `/plugin/smartreel/api/v1/…`, plus two web-UI panels:

- **Build Order page → "SmartReel" panel** — send the build's BOM to the
  rack as a pick job, watch per-item progress, remove/resend.
- **Rack location page → "SmartReel HMI" panel** — provisioning QR
  (`SRPROV1:` payload with URL + dedicated API token). Scan it from the
  HMI instead of typing credentials.

## Structure

```
smart_reel/
  core.py       # plugin class: settings, URL routes, UI panels
  api.py        # DRF views (thin HTTP layer, op_id idempotency)
  services.py   # domain logic against InvenTree models
  static/panel.js  # both web panels (no build step)
```

## Multiple racks

One instance drives many independent SmartReel units. Each rack is its
own StockLocation tagged `metadata['smartreel'] = {"is_rack": true}`;
provisioning binds the issued API token to that rack
(`token.metadata['smartreel_rack']`), so the HMI's token alone tells the
plugin which rack it's driving — no HMI/wire change. An unbound token is
refused (409). Provision each rack from its stock-location page → the
"SmartReel HMI" panel. `./test-multirack.sh` covers rack isolation.

## Behaviour notes

- **Slots** are StockLocation children of a rack location, carrying
  `metadata['smartreel'] = {'slot': n}`; created/grown by
  `POST /rack/register` (which targets the token's rack).
- **Picks are whole-reel** `StockItem.move()` transfers (audit-trailed) to
  the staging location — no quantity math.
- **Pick jobs** live in the source Build's `metadata['smartreel']`
  (with a target rack pk); job status is derived
  (`pending`/`partial`/`done`).
- **Anomalies** append to the rack location's
  `metadata['smartreel_anomalies']` (capped at 200).

## Settings (InvenTree → Settings → Plugins → Smart Reel)

| Setting | Meaning |
|---|---|
| Staging location | default destination for picked reels (per-rack override via metadata) |
| Pulled location | destination for anomaly-cleared reels (per-rack override via metadata) |

Racks are not a setting — provision each from its stock-location page.

InvenTree must have **plugin URL integration enabled**
(`ENABLE_PLUGINS_URL`) for the API to be reachable.

## Dev install

Installed (editable) into the local InvenTree docker instance at
`~/Desktop/Projects/inventree/` — see the README there for the
mount/reload workflow. Restart `inventree-server` + `inventree-worker`
after code changes. `./test-live.sh` exercises the full contract against
that instance.
