# SmartReel — Operator User Manual

A plain-language guide to using the SmartReel touchscreen for everyday work. No
technical background needed. If you're setting up a rack for the first time, see
the [Concept of Operations, Commissioning](01-concept-of-operations.md#41-commissioning-a-rack)
first.

## What SmartReel does for you

SmartReel is a storage rack for component reels with a touchscreen on it. It
remembers which reel is in which slot, lights up the right slot to guide you,
and keeps your InvenTree inventory up to date automatically. You mostly just
**scan, look for the lit slot, and place or pick the reel.**

## The Home screen

The Home screen has five big buttons:

| Button | Use it to… |
|---|---|
| **Load** | Put a new reel into the rack. |
| **View** | Browse what's in the rack, find a part, or pick one reel. |
| **Pick** | Work through a pick job for a build. |
| **Rack** | See the whole rack at a glance (a colour-coded grid of slots). |
| **Settings** | Network setup, firmware updates, diagnostics, slot setup. |

### The online indicator

There's an online/offline indicator on the status bar. When it shows
**online**, everything works. When it shows **offline**, the rack has lost its
connection to InvenTree — see [If the rack is offline](#if-the-rack-is-offline).

### What the slot lights mean

| Light | Meaning |
|---|---|
| Off | Normal occupied or empty slot. |
| Blue | This is your target — place a reel here, or pick from here. |
| Green | Just picked / completed. |
| Amber | Attention: a reel needs to be removed (it was moved in InvenTree). |
| Red | Error with this slot. |

When a slot has been widened by pulling dividers, **all** the LEDs in that wide
slot light up together.

---

## Task: Load a new reel (put-away)

Do this after a reel has been received into InvenTree.

1. Tap **Load** on the Home screen.
2. **Scan the reel's QR code** with the scanner. (No scanner? Tap to enter the
   code by hand.)
3. The screen shows the **part name and quantity**.
   - If it says **"already in slot N"**, that reel is already stored — you're
     done.
4. The rack lights up **every empty slot** in blue. These are your choices.
5. **Drop the reel into any lit slot.** The light turns off and the reel is now
   stored. SmartReel records its location in InvenTree for you.

To back out before placing the reel, tap **Cancel**.

---

## Task: Find a part

1. Tap **View**.
2. Scroll to the part (or search).
3. Tap **Find** next to it. The reel's slot flashes for a few seconds so you can
   spot it.

---

## Task: Pick one reel

1. Tap **View** and find the part.
2. Tap **Pick**. The reel's slot lights blue.
3. **Lift the reel out.** The light turns off and SmartReel records the removal
   in InvenTree (the reel moves to the Staging location).

To stop before removing the reel, tap **Cancel** — nothing changes.

> Picking only works while the rack is **online**. If it's offline, the Pick
> button is disabled and a banner explains why.

---

## Task: Work a pick job (kitting a build)

A pick job is a list of parts someone sent to this rack from a Build Order in
InvenTree.

1. Tap **Pick**. You'll see the available jobs.
   - A job marked **partially picked** has a **Resume** button — you can finish
     it later.
2. Tap a job to open it. The rack lights up **every slot** that holds a part the
   job still needs.
3. For each part:
   - **Lift a reel** from any lit slot for that part, **or**
   - tap the **Picked** button next to the item.
   That item is checked off.
4. When everything is checked off, the job is **done** and disappears.

If you tap **Cancel** partway through, the items you already picked stay picked.
The job shows as *partially picked* and you can come back and Resume it.

Parts that aren't in this rack show up as **unfulfillable** so you know to get
them elsewhere.

---

## Task: See the whole rack

Tap **Rack** for a colour-coded grid of every slot — handy for a quick overview
of what's occupied, empty, or needs attention.

---

## Handling problems

### A slot is lit amber and the screen says "Stock moved in InvenTree"

Someone moved that reel's record in InvenTree without taking the reel out of the
rack. **Remove the reel from the amber slot.** The light clears and everything
is back in sync. (You don't need to scan anything — InvenTree already knows.)

### I pulled a reel without picking it

SmartReel notices a reel that left without a pick, logs it, and moves that stock
to the **Pulled** location in InvenTree so it isn't lost. If you didn't mean to,
re-load the reel with the **Load** flow.

### A slot is red

There's a problem with that slot or its sensor. Try reseating the reel; if it
persists, tell your maintainer and run **Settings → Self-test**.

### If the rack is offline

The online indicator shows **offline** when the rack can't reach InvenTree.
While offline:

- **You can still load reels** (the change is saved and sent automatically once
  the connection comes back).
- **You cannot pick** — this is on purpose, so the inventory never gets out of
  step. The Pick button is disabled.
- SmartReel keeps trying to reconnect on its own. Once it's back online, any
  pending changes are sent automatically.

If it stays offline, check the WiFi/network and the InvenTree connection in
**Settings → Network**.

### Re-connecting / re-provisioning the rack

If credentials need refreshing (e.g. the token was rotated in InvenTree):

1. In InvenTree, open this rack's location page → **SmartReel HMI** panel →
   get the setup QR (rotate the token if needed).
2. On the HMI: **Settings → Network → Scan setup code**, and scan the screen.
   The rack tests the connection right away.

---

## Tips

- **Scan first, then place.** The lit slots only appear after a successful scan.
- **Any lit slot works** for loading — pick whichever is convenient.
- **The reel's presence button is what counts.** SmartReel reacts when you
  physically place or remove a reel, not just when you tap the screen.
- **Wide slots** (dividers pulled) hold larger reels; the whole group lights up
  as one.
- **Cancel is always safe** before you move a reel — it changes nothing.
</content>
