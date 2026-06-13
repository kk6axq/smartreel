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

## Behaviour notes

- **Slots** are StockLocation children of the configured rack location,
  carrying `metadata['smartreel'] = {'slot': n}`; created/grown by
  `POST /rack/register`.
- **Picks are whole-reel** `StockItem.move()` transfers (audit-trailed) to
  the staging location — no quantity math.
- **Pick jobs** live in the source Build's `metadata['smartreel']`; job
  status is derived (`pending`/`partial`/`done`).
- **Anomalies** append to the rack location's
  `metadata['smartreel_anomalies']` (capped at 200).

## Settings (InvenTree → Settings → Plugins → Smart Reel)

| Setting | Meaning |
|---|---|
| Rack location | structural location representing the SmartReel unit |
| Staging location | default destination for picked reels |
| Pulled location | destination for anomaly-cleared reels |

InvenTree must have **plugin URL integration enabled**
(`ENABLE_PLUGINS_URL`) for the API to be reachable.

## Dev install

Installed (editable) into the local InvenTree docker instance at
`~/Desktop/Projects/inventree/` — see the README there for the
mount/reload workflow. Restart `inventree-server` + `inventree-worker`
after code changes. `./test-live.sh` exercises the full contract against
that instance.
