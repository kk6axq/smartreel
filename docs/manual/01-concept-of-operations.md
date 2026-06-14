# SmartReel — Concept of Operations

## 1. What SmartReel is

SmartReel is a smart storage rack for reels of surface-mount electronic
components, designed to work hand-in-hand with InvenTree, an open-source
inventory management system. A physical SmartReel rack is made of one or more
**reel modules**, each providing a row of slots. Every slot:

- senses whether a reel is physically present (a button under the reel),
- has an addressable RGB LED to guide the operator, and
- can be widened by pulling **dividers** between slots to fit larger reels.

A touchscreen **HMI** (Human–Machine Interface) mounted on the rack is the
operator's control panel. The HMI talks to the rack hardware over an RS485
serial link and to InvenTree over HTTPS. InvenTree stays the single source of
truth for inventory; SmartReel records *where* each reel physically lives
("SmartReel Rack A, Slot 32") and guides the operator to the right slot.

The goal is to remove the two annoying parts of bench-stock management:
remembering where a reel is, and keeping the digital inventory in step with the
physical one. The operator scans, the rack lights up, and the inventory updates
itself.

## 2. Who uses it

- **Shop-floor operator / technician** — the everyday user. Stores received
  reels, picks single reels, and works through pick jobs for builds. Interacts
  almost entirely through the HMI touchscreen and a QR scanner. The
  [Operator User Manual](06-operator-manual.md) is written for this person.
- **Inventory / production manager** — works in InvenTree's web UI. Receives
  stock, creates build orders, and sends pick jobs to a rack from the Build
  Order page. Provisions new racks from the InvenTree location page.
- **Installer / maintainer** — sets up a rack: wires it, commissions the slot
  layout on the HMI, provisions it against InvenTree, and performs firmware
  updates.

The target environment is a small-to-medium business or a serious hobbyist
doing PCB assembly, who already runs InvenTree and wants to automate the
monotonous parts of inventory handling.

## 3. The pieces, at a glance

```
   Operator                InvenTree manager
      |                            |
   [touchscreen]              [web browser]
      |                            |
  +-------+   RS485   +--------+   HTTPS   +-----------------------+
  |  HMI  |<--------->| Core   |           |     InvenTree         |
  | ESP32 |           | PCB    |           |  + SmartReel plugin   |
  |  -S3  |           | RP2040 |           +-----------------------+
  +-------+           +--------+
      |                  |
      | HTTPS            | drives reel modules (LEDs + sensors)
      v                  v
  InvenTree         [Reel modules: slots, dividers, LEDs]
```

See the [Software Architecture](03-software-architecture.md) and
[Hardware](02-hardware.md) docs for the full breakdown.

## 4. End-to-end workflows

The behavioural source of truth is `docs/user-stories.md`. The workflows below
describe what actually happens across the HMI, the rack hardware, and InvenTree.

### 4.1 Commissioning a rack

Done once when a rack is installed, and again whenever its physical layout
changes.

1. **Wire and power the rack.** Reel modules chain off the Core PCB's ports;
   the HMI connects to the Core over RS485. (See [Hardware](02-hardware.md).)
2. **Provision against InvenTree.** In InvenTree, open the stock location that
   represents this rack, open the **SmartReel HMI** panel, and click
   *Provision*. The plugin tags that location as a rack, mints a dedicated API
   token bound to it, and shows a **setup QR code** (`SRPROV1:` payload with
   the plugin URL + token).
3. **Scan the setup code on the HMI.** Settings → Network → *Scan setup code*.
   The HMI stores the URL and token and immediately runs a health check. From
   now on the rack's identity rides on its token — no rack number to configure.
4. **Commission the slot layout.** On first power-up the HMI follows the live
   hardware topology it reads from the Core (how many modules are on each port,
   where the dividers are). The installer reviews this in
   Settings → Slots / Dividers and **commits** it. After commit, the HMI
   validates the live hardware against the committed layout and flags any
   deviation as an anomaly.
5. **Register slots with InvenTree.** On first successful contact the HMI calls
   `POST /rack/register`, which creates one InvenTree sub-location per physical
   slot ("Slot 01" … "Slot NN") under the rack location.

> Multiple racks: one InvenTree instance can drive many racks. Each is
> provisioned separately and identified by its own token. There is no global
> "rack number" setting.

### 4.2 Stocking reels (put-away / load)

When new reels arrive, the operator first receives them into InvenTree
normally (so each reel becomes a StockItem with a QR label). Then they store
them physically:

1. Operator walks to the HMI, taps **Load**, and scans the reel's QR code.
2. The HMI sends the scanned code to `POST /barcode/resolve`. InvenTree
   resolves it to the StockItem and returns the part name and quantity. If the
   reel is already recorded in a slot, the HMI says "already in slot N".
3. The HMI shows the part details and lights every **empty** slot as a
   candidate location.
4. The operator drops the reel into any lit slot. The slot's presence sensor
   fires; after a short debounce the HMI registers that slot as the reel's
   home and queues `POST /rack/slots/{n}/assign`. InvenTree moves the StockItem
   into that slot's sub-location (with an audit trail).
5. The slot LED returns to normal; the load is complete.

If the rack is offline, loads against the on-SD offline catalog stay local and
are reconciled later. Operations that change inventory are queued and retried.

### 4.3 Picking a single reel

For grabbing one whole reel.

1. Operator taps **View**, scrolls to the part (or uses *Find*, which flashes
   the slot LEDs for a few seconds), and taps **Pick**.
2. The HMI lights that slot. The operator can tap **Cancel** any time before
   removing the reel.
3. The operator lifts the reel out. The presence sensor releases; the HMI
   queues a **whole-reel pick** (`POST /rack/slots/{n}/pick`). InvenTree
   transfers the entire StockItem to the configured **Staging** location (or a
   per-pick destination). No quantity math — the whole reel left the rack.
4. The LED turns off; the slot is now logically empty.

Picking is **disabled while the rack is offline** (a banner explains why) to
keep SmartReel from drifting out of sync with InvenTree.

### 4.4 Job picking (kitting a build)

For assembling all the parts a build order needs.

1. In InvenTree, the manager opens a Build Order, opens the **SmartReel**
   panel, picks the target rack, and clicks *Send to SmartReel*. The plugin
   turns the build's BOM into a **pick job** (one item per part) stored on the
   build, scoped to that rack.
2. On the HMI, the operator opens **Pick**, selects the job, and the rack
   lights every slot that holds a part the job still needs.
3. For each item, the operator removes a reel from any lit slot (or taps the
   on-screen *Picked* button). The HMI reports `pickjobs/{id}/items/{idx}/pick`;
   InvenTree marks that item picked and moves the reel to the job's destination.
4. When all items are picked the job is **done** and disappears from the list.
5. **Partial picks resume.** If the operator cancels partway, already-picked
   items stay picked server-side; the job shows as *partially picked* with a
   **Resume** button and can be finished later. Cancel is local-only.

Items whose part isn't currently in the rack are shown as unfulfillable rather
than hidden.

### 4.5 Inventory reconcile (background)

InvenTree is the source of truth, and stock can move there without anyone
touching the rack (e.g. someone transfers a reel's StockItem in the web UI).
SmartReel detects and resolves these drifts:

- Every ~60 seconds (and on demand) the HMI fetches a full rack snapshot
  (`GET /rack`) and compares it to the physical truth from the sensors.
- **Moved in InvenTree but still physically present** → the HMI lights the slot
  amber and shows a "Stock moved in InvenTree" prompt instructing removal.
  Removing the reel resolves it (no inventory action — InvenTree already moved
  it).
- **Logically present but physically absent** (a reel yanked without a pick) →
  the HMI logs an anomaly and clears the slot to the **Pulled** location so the
  stock isn't lost.
- **Both present** → the HMI adopts InvenTree's part/quantity for that slot
  (boot-time sync).

Reconcile is skipped for slots the operator is actively working (a lit pick
target, a just-picked slot) and when the Core hardware mirror is invalid.

### 4.6 Firmware update

Two independent update paths exist (see the [API Reference](04-api-reference.md)
for the wire/partition detail):

- **HMI self-update** — the ESP32-S3 flashes a new application image from a
  file on its SD card into the inactive OTA partition, then reboots into it.
- **Core update over RS485** — the HMI streams a new RP2040 image to the Core in
  512-byte chunks (`FW_BEGIN` → `FW_CHUNK` × N → `FW_VERIFY` → `FW_COMMIT`),
  SHA-256 verified, with an A/B-slot bootloader that rolls back automatically
  if the new image fails to confirm after boot.

## 5. Online / offline philosophy

The HMI continuously health-checks InvenTree (every 15 s while up, every 5 s
while down) and shows an online/offline indicator. The guiding rule:

- **Reads and put-away** degrade gracefully offline (the HMI can show the SD
  offline catalog, and assigns queue for later).
- **Picks are blocked offline** to guarantee the rack never reports a removal
  that InvenTree doesn't know about.
- Every inventory-changing action is queued with an idempotency key (`op_id`)
  and retried until it succeeds, so a flaky network can't lose or double-apply
  a move.
</content>
