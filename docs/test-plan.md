# SmartReel test plan — 2026-06-12 build

Covers the work from `docs/session-notes-2026-06-12.md`: the rewritten
HMI↔plugin contract (`docs/hmi-plugin-api.md`), the real InvenTree plugin,
the realigned mock, and the HMI firmware sync layer. Ordered so each phase
builds confidence for the next; you can stop at a phase boundary and still
have learned something coherent.

Legend: ☐ = do it, **PASS:** = what you must observe.

---

## Phase 0 — Automated checks (5 min, no hardware)

These should already be green; re-run them first so any later failure is
known to be hardware/integration, not regression.

- ☐ `mock-inventree/smoke-test.sh`
  **PASS:** every line `ok`, final `ALL OK`.
- ☐ InvenTree stack up (`docker compose up -d` in `~/Desktop/Projects/inventree`),
  then `inventree-plugin/test-live.sh`
  **PASS:** 32 × `ok`, final `ALL OK`.
- ☐ `cd esp32-hmi && pio run`
  **PASS:** `[SUCCESS]`.

## Phase 1 — Web / plugin UI (browser, ~15 min)

I couldn't eyeball the browser side; this is the part most in need of
human eyes.

- ☐ Open http://inventree.localhost, log in (admin/inventree).
- ☐ Settings → Plugins → Smart Reel: confirm the three location settings
  show pickers and are set (SmartReel Rack / Staging / Pulled — the live
  test seeded them).
- ☐ Navigate to the **SmartReel Rack** stock location page.
  **PASS:** a "SmartReel HMI" panel renders a QR code + payload text
  starting `SRPROV1:`, with a "Rotate token" button. Rotate once —
  **PASS:** payload changes, no error.
- ☐ Navigate to the test build order (Manufacturing → Build Orders →
  "SmartReel test build").
  **PASS:** a "SmartReel" panel renders. It will show the pick job from
  the live test (status `partial`, item 0 picked ✅, item 1 listed with
  qty + location/"not in rack").
- ☐ Click **Remove from SmartReel** → panel returns to the empty state.
  Click **Send to SmartReel** → job reappears as `pending`, both items
  unpicked.
  **PASS:** no console errors, states update without reload.
- ☐ Cosmetics: panel readable in both InvenTree light/dark theme.
- Cleanup note: the live test seeds parts (R-10K-0805, C-100N-0805,
  ASM-TEST-1), stock items (a few per rerun), locations, and one build
  order. Delete at will; reruns reseed what they need.

## Phase 2 — HMI ⇄ mock server (bench, no InvenTree, ~30 min)

The mock speaks HTTPS already, so this exercises the full firmware path
(TLS, provisioning, all flows) with controllable data.

Setup:
- ☐ Flash the HMI (`scripts/` flash helper or `pio run -t upload`).
- ☐ Start the mock: `cd mock-inventree && ./run.sh` (HTTPS :8443).
- ☐ Open the dashboard from a browser **via the LAN IP**
  (`https://<lan-ip>:8443/`), set token `dev-token`.

Provisioning (replaces typing URL/token):
- ☐ Dashboard → "HMI provisioning" → Show setup QR.
- ☐ HMI: Settings → Network → **Scan setup code** → scan the monitor.
  **PASS:** status says applied + testing; the InvenTree card shows the
  URL; status line goes "Online: smartreel-mock…"; the status-bar pill
  turns ONLINE within ~15 s.

Boot sync / reconcile:
- ☐ Reboot the HMI with the mock running.
  **PASS:** within ~1 min the View screen lists parts matching the
  dashboard's rack grid (server metadata adopted), assuming the bench
  Core is attached and reels are physically present in those slots.
  (With no Core attached, reconcile is intentionally skipped.)

Load flow (user story 1):
- ☐ Scan one of the printed `MOCK-PART-…` QR sheets on the Load screen.
  **PASS:** part name **and qty** shown on the locked card.
- ☐ Place reel → insert into a lit slot.
  **PASS:** HMI returns Home; dashboard shows that stock item now in the
  slot (slot turns occupied) within a second or two.
- ☐ Scan the same code again.
  **PASS:** "already in slot N" message, no duplicate load.

Single pick (user story 3):
- ☐ View → Pick on a row → slot lights blue → remove the reel.
  **PASS:** row disappears; dashboard shows the slot empty and the stock
  item's location = 99 (Staging). Cancel path: arm then tap Cancel —
  LED off, nothing moved.

Pick job (user story 4):
- ☐ Mock seeds jobs BO-0042/3/4. Pick screen → Start.
  **PASS:** all locatable items' slots light; removing a reel marks its
  item picked on both HMI and dashboard (job goes `partial` → `done`).
- ☐ Partial + resume: start a job, pick ONE item, Cancel job.
  **PASS:** dashboard keeps that one item picked; HMI pick list shows
  "partially picked" + **Resume**; resuming lights only remaining items.

Anomalies:
- ☐ Yank an inventoried reel with no job active.
  **PASS:** modal raised; dashboard anomaly log gains a `removed` entry;
  the stock item moves to location 98 (pulled).
- ☐ Insert a random reel without scanning.
  **PASS:** "added" warning + anomaly log entry; removing it clears the
  warning without a fault.

Moved-in-InvenTree reconcile (background story):
- ☐ With a reel physically in slot N and visible on the dashboard, fake a
  server-side move: `curl -k -H 'Authorization: Token dev-token' -X POST
  https://localhost:8443/api/v1/rack/slots/N/pick -d '{"op_id":"manual-1"}'
  -H 'Content-Type: application/json'` (whole-reel pick = stock left the
  rack logically).
  **PASS:** within 60 s the HMI lights slot N amber + raises "Stock moved
  in InvenTree"; physically removing the reel resolves it with no
  further inventory action.

Offline behaviour (connectivity story):
- ☐ Stop the mock (Ctrl-C).
  **PASS:** within ~30 s the status pill drops to offline; View shows the
  warning banner with Pick buttons disabled; Pick screen disables
  Start/Resume. Find (LED locate) still works.
- ☐ Restart the mock.
  **PASS:** pill returns ONLINE within ~15 s without touching the HMI.
- ☐ Retry queue: with the mock stopped, do a LOAD via the offline SD
  catalog — confirm it stays local (no crash). Then (mock running) do a
  load + immediately kill the mock before the assign lands; restart the
  mock within a minute.
  **PASS:** the dashboard eventually shows the assign (op queue retried).

## Phase 3 — HMI ⇄ real InvenTree (~30 min)

✅ Prereq satisfied (2026-06-12): Caddy now serves HTTPS on
`inventree.localhost` and the LAN IPs (192.168.1.235 / .243) with its
internal CA (`default_sni 192.168.1.235` handles the HMI's no-SNI
connections). The plugin's **HMI base URL** setting is set to
`https://192.168.1.235`, so the provisioning QR embeds a reachable
HTTPS address — `inventree-plugin/set-hmi-url.sh <url>` updates it if
the IP/domain changes (the Caddyfile site list must match).

- ☐ Provision the HMI from the **plugin's** QR panel (rack location page).
  Check the payload URL is the HTTPS LAN base + `/plugin/smartreel`.
  **PASS:** HMI goes online against real InvenTree.
- ☐ Print an InvenTree QR label for a real stock item (or display the
  `{"stockitem": <pk>}` barcode on screen) and run the load flow.
  **PASS:** stock item lands in `SmartReel Rack/Slot NN` in InvenTree,
  with a tracking entry "SmartReel: loaded into slot".
- ☐ Single pick → stock item in Staging with tracking entry.
- ☐ Send a job from the Build Order panel → run it on the HMI →
  panel shows live progress; picked reels in Staging.
- ☐ Move a stock item out of its slot using InvenTree's own UI →
  amber "remove me" on the HMI within 60 s.
- ☐ Yank a reel → anomaly appears in the rack location's metadata
  (admin → stock location → metadata `smartreel_anomalies`) and stock
  moves to Pulled.

## Phase 4 — Hardware / UI regression sweep (~20 min)

Mostly unchanged code, but the font bump + new screens touch everything
visual, and inv_sync adds a new background task.

- ☐ Font/layout audit on every screen (Home, Rack, View, Pick list/active,
  Load all three views, Settings + each sub-screen, modals, text-entry
  keyboard, status bar).
  **PASS:** nothing clipped/overlapping; status bar title fits at 46 px;
  rows fit two lines; tiles look right in the new 3×2 layout.
- ☐ Rename check: tile + title say **Settings**, status-bar title "Rack"
  on the grid screen.
- ☐ RGB tearing: watch the display while the HMI is online (health poll
  every 15 s + rack fetch every 60 s run TLS on APP_CPU).
  **PASS:** no tearing/stutter during background sync, scrolling lists
  stay smooth.
- ☐ Serial console: `stats` still responds; no watchdog resets or stack
  canaries over a 15-minute idle-online soak (`inv-sync` task is new —
  watch for its logs misbehaving).
- ☐ SD persistence: power-cycle mid-session → rack contents restore, then
  reconcile corrects them against the server.
- ☐ Self Test screen: buttons card still swallows input events (no
  anomaly modals while testing), LED tests work.
- ☐ Divider pull/insert still rebuilds the rack (pre-commission) or
  raises the divider anomaly (post-commission).

## Known gaps / expected failures

- Cert pinning: HMI still `setInsecure()`; SRPROV1 `f` field parsed but
  unused. Fine for bench, fix before anything ships. (Caddy's internal
  CA rotates leaf certs ~every 12 h, so when pinning lands it should pin
  the CA, not the leaf.)
- LAN IPs in the Caddyfile + HMI_URL setting are DHCP leases — a lease
  change breaks HMI connectivity until both are updated (consider a DHCP
  reservation or a real domain).
- Fonts were doubled vs the original build (24/28 body, 36 menu/prompts,
  48 tiles) — expect some clipping on dense screens (Self Test cards,
  Network scan list, text-entry modal); log what needs shrinking.
- Job list won't refresh while a job is active (by design — indices).
- If the HMI has >16 queued ops while offline, further ops drop with a
  serial log (`inv_sync: op queue full`) — reconcile heals it later.
- Browser panel render (Phase 1) was never seen by me — most likely
  place for a dumb bug (CSS, data shape in `data.context`).
