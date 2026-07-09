"""Domain logic for the SmartReel plugin.

Everything that touches InvenTree models lives here; api.py stays a thin
HTTP layer. The wire shapes produced here are specified in
docs/hmi-plugin-api.md (SmartReel repo) — keep them in sync with the mock.

Multi-unit model
----------------
The plugin supports multiple independent SmartReel racks on one InvenTree
instance. Each rack is a StockLocation tagged with
metadata['smartreel'] = {"is_rack": true, "staging": <pk>?, "pulled": <pk>?}.
Slots are child locations of a rack, tagged metadata['smartreel'] = {"slot": n}.

A rack's identity rides on the HMI's API token: provisioning sets
token.get_metadata('smartreel_rack') = <rack location pk>, so every
rack-scoped request resolves its rack from request.auth — no rack id in
the URL and no HMI/wire change. A token that isn't bound to a rack is
rejected; every unit (including the first) is provisioned explicitly.

Conventions:
- Wire part ids are the part's IPN when set, else its pk as a string.
- Picks are whole-reel transfers (StockItem.move), never quantity math.
- Staging/pulled are per-rack (rack metadata) with a global-setting fallback.
"""
from __future__ import annotations

import logging

from django.core.cache import cache
from django.db import transaction
from django.utils import timezone

logger = logging.getLogger("inventree")

METADATA_KEY = "smartreel"          # on slot locations: {"slot": n}
                                    # on rack locations: {"is_rack": true, "staging", "pulled"}
                                    # on build orders (legacy): a single pick job
JOBS_KEY = "smartreel_jobs"         # on build orders: {str(rack_pk): job} (multi-rack)
TOKEN_RACK_KEY = "smartreel_rack"   # on an ApiToken: the rack location pk it controls
OP_CACHE_TTL = 600  # seconds; mirrors the mock's idempotency window


# ---------------------------------------------------------------------------
# op_id idempotency
# ---------------------------------------------------------------------------

def op_lookup(op_id: str | None):
    if not op_id:
        return None
    return cache.get(f"smartreel:op:{op_id}")


def op_remember(op_id: str | None, resp: dict) -> None:
    if op_id:
        cache.set(f"smartreel:op:{op_id}", resp, OP_CACHE_TTL)


# ---------------------------------------------------------------------------
# Plugin / settings access
# ---------------------------------------------------------------------------

def get_plugin():
    from plugin import registry

    return registry.get_plugin("smartreel")


def _loc_by_pk(pk):
    from stock.models import StockLocation

    if not pk:
        return None
    try:
        return StockLocation.objects.get(pk=int(pk))
    except (StockLocation.DoesNotExist, ValueError, TypeError):
        return None


def _location(setting_key: str):
    """StockLocation referenced by a plugin setting, or None."""
    return _loc_by_pk(get_plugin().get_setting(setting_key))


class ConfigError(Exception):
    """A required plugin setting is missing/invalid. Maps to HTTP 409."""


class SlotConflict(Exception):
    """Maps to HTTP 409."""


def require(loc, what: str):
    if loc is None:
        raise ConfigError(
            f"SmartReel plugin setting '{what}' is not configured "
            f"(InvenTree → Settings → Plugins → Smart Reel)"
        )
    return loc


# ---------------------------------------------------------------------------
# Racks (multi-unit)
# ---------------------------------------------------------------------------

def is_rack_location(loc) -> bool:
    if loc is None:
        return False
    meta = loc.get_metadata(METADATA_KEY) or {}
    return bool(meta.get("is_rack"))


def mark_rack(loc):
    """Tag a location as a SmartReel rack (idempotent). Returns it."""
    meta = loc.get_metadata(METADATA_KEY) or {}
    if not meta.get("is_rack"):
        meta["is_rack"] = True
        loc.set_metadata(METADATA_KEY, meta)
    return loc


def list_rack_locations() -> list:
    """Every SmartReel rack: locations tagged is_rack (i.e. provisioned)."""
    from stock.models import StockLocation

    out = []
    for loc in StockLocation.objects.exclude(metadata__isnull=True):
        meta = loc.get_metadata(METADATA_KEY) or {}
        if meta.get("is_rack"):
            out.append(loc)
    return out


def rack_info_list() -> list[dict]:
    """Lightweight rack list for the web panel's rack selector."""
    out = []
    for rack in list_rack_locations():
        out.append({
            "location_id": rack.pk,
            "name": rack.pathstring or rack.name,
            "n_slots": len(slot_map(rack)),
        })
    out.sort(key=lambda r: r["name"])
    return out


def rack_for_token(token):
    """Resolve the rack an HMI token is bound to, or None if unbound."""
    if token is not None and hasattr(token, "get_metadata"):
        return _loc_by_pk(token.get_metadata(TOKEN_RACK_KEY))
    return None


def staging_for(rack):
    meta = rack.get_metadata(METADATA_KEY) or {}
    return _loc_by_pk(meta.get("staging")) or _location("STAGING_LOCATION")


def pulled_for(rack):
    meta = rack.get_metadata(METADATA_KEY) or {}
    return _loc_by_pk(meta.get("pulled")) or _location("PULLED_LOCATION")


# ---------------------------------------------------------------------------
# Slot locations (scoped to a rack)
# ---------------------------------------------------------------------------

def slot_map(rack) -> dict[int, object]:
    """{slot_num: StockLocation} for every registered slot under `rack`."""
    from stock.models import StockLocation

    if rack is None:
        return {}
    out: dict[int, object] = {}
    for loc in StockLocation.objects.filter(parent=rack):
        meta = loc.get_metadata(METADATA_KEY) or {}
        slot = meta.get("slot")
        if isinstance(slot, int):
            out[slot] = loc
    return out


def ensure_slots(rack, n_slots: int, user=None) -> dict[int, object]:
    """Create missing slot sub-locations 1..n_slots under `rack`."""
    from stock.models import StockLocation

    mark_rack(rack)   # registering implicitly designates the location a rack
    existing = slot_map(rack)
    missing = [n for n in range(1, n_slots + 1) if n not in existing]
    if not missing:
        return existing

    # Collapse the N+1 into one transaction. NOTE: StockLocation is an MPTT
    # tree (InvenTreeTree) — its save() maintains tree_id/lft/rght/level and
    # pathstring and runs a partial tree rebuild per node, none of which
    # bulk_create() performs. bulk_create() would therefore corrupt the
    # location tree (and leave pathstring empty), so we keep StockLocation
    # .create() / set_metadata() per slot but wrap the whole batch in a single
    # transaction.atomic(). That turns hundreds of individual autocommits into
    # one committed transaction (a large round-trip reduction) while preserving
    # exact MPTT/pathstring/metadata behavior.
    with transaction.atomic():
        for n in missing:
            loc = StockLocation.objects.create(
                name=f"Slot {n:02d}",
                parent=rack,
                description=f"SmartReel slot {n}",
            )
            loc.set_metadata(METADATA_KEY, {"slot": n})
            existing[n] = loc
            logger.info("smartreel: rack %s created slot %s (pk=%s)", rack.pk, n, loc.pk)
    return existing


def stock_in(location) -> object | None:
    """The (single) in-stock StockItem homed at `location`, or None."""
    from stock.models import StockItem

    items = StockItem.objects.filter(location=location, quantity__gt=0)
    return items.first()


# ---------------------------------------------------------------------------
# Wire renderers
# ---------------------------------------------------------------------------

def part_wire_id(part) -> str:
    return part.IPN or str(part.pk)


def part_by_wire_id(wire_id: str):
    """Reverse of part_wire_id: IPN match first, then pk."""
    from part.models import Part

    part = Part.objects.filter(IPN=wire_id).first()
    if part is None and wire_id.isdigit():
        part = Part.objects.filter(pk=int(wire_id)).first()
    return part


def render_part(part) -> dict:
    pkg = ""
    try:
        pkg = str(part.get_parameter("Package") or "")
    except Exception:
        pass
    mfg = ""
    try:
        mp = part.manufacturer_parts.first()
        if mp and mp.manufacturer:
            mfg = mp.manufacturer.name
    except Exception:
        pass
    return {
        "id": part_wire_id(part),
        "name": part.full_name,
        "pkg": pkg[:9],
        "mfg": mfg[:15],
    }


def render_stock(item, slots: dict[int, object]) -> dict:
    """`slots` is the requesting rack's slot map; slot_num is relative to it."""
    slot_num = None
    for n, loc in slots.items():
        if item.location_id == loc.pk:
            slot_num = n
            break
    barcode = item.barcode_data or ""
    if not barcode:
        try:
            barcode = item.barcode  # generated InvenTree barcode
        except Exception:
            barcode = ""
    return {
        "id": item.pk,
        "part": render_part(item.part),
        "qty": int(item.quantity),
        "batch": item.batch or "",
        "barcode": barcode,
        "location_id": item.location_id or 0,
        "slot_num": slot_num,
    }


# ---------------------------------------------------------------------------
# Rack snapshot + slot ops (all scoped to a resolved `rack`)
# ---------------------------------------------------------------------------

def rack_snapshot(rack) -> dict:
    slots = slot_map(rack)
    out = []
    for n in sorted(slots):
        loc = slots[n]
        item = stock_in(loc)
        out.append({
            "slot": n,
            "location_id": loc.pk,
            "stock": render_stock(item, slots) if item else None,
        })
    jobs = [j for j in list_jobs(rack) if j["status"] != "done"]
    return {
        "location_id": rack.pk,
        "n_slots": len(out),
        "slots": out,
        "pickjobs_available": len(jobs),
        # Pending "locate" requests (InvenTree locate button); the HMI lights
        # these slots and acks the ids via POST /rack/locates/ack.
        "locates": locate_requests(rack),
    }


def occupancy_rev(rack) -> str:
    """Cheap occupancy fingerprint for the HMI's fast-poll change detector
    (review item 6). One query over the rack's slot stock; the HMI polls this
    every ~5s and only runs the full GET /rack reconcile when `rev` changes,
    so a reel moved out of a slot in InvenTree is picked up within ~10s
    without the cost of a full snapshot each poll.
    """
    import hashlib

    from stock.models import StockItem

    slots = slot_map(rack)
    slot_for_loc = {loc.pk: n for n, loc in slots.items()}
    rows = []
    if slot_for_loc:
        for loc_id, pk, qty in StockItem.objects.filter(
            location_id__in=list(slot_for_loc), quantity__gt=0
        ).values_list("location_id", "pk", "quantity"):
            rows.append((slot_for_loc[loc_id], pk, int(qty)))
    rows.sort()
    # Fold the pending locate queue in too, so a web-UI "locate" is also
    # surfaced within the fast-poll window, not only at the slow reconcile.
    locs = locate_requests(rack)
    loc_sig = (max((e.get("id", 0) for e in locs), default=0), len(locs))
    payload = repr((rows, loc_sig)).encode()
    return hashlib.sha1(payload).hexdigest()[:16]


def assign_slot(rack, slot_num: int, stock_item_id: int, user) -> dict:
    from stock.models import StockItem

    slots = slot_map(rack)
    loc = slots.get(slot_num)
    if loc is None:
        raise LookupError(f"slot {slot_num} not found")
    try:
        item = StockItem.objects.get(pk=stock_item_id)
    except StockItem.DoesNotExist:
        raise LookupError(f"stock item {stock_item_id} not found")

    current = stock_in(loc)
    if current is not None and current.pk != item.pk:
        raise SlotConflict(f"slot {slot_num} already holds stock item {current.pk}")

    item.move(loc, "SmartReel: loaded into slot", user)
    item.refresh_from_db()
    return {"slot": slot_num, "stock": render_stock(item, slots)}


def pick_slot(rack, slot_num: int, user, destination_id: int | None = None) -> dict:
    """Whole-reel pick: transfer the slot's StockItem to the destination."""
    slots = slot_map(rack)
    loc = slots.get(slot_num)
    if loc is None:
        raise LookupError(f"slot {slot_num} not found")
    item = stock_in(loc)
    if item is None:
        raise SlotConflict(f"slot {slot_num} is empty")

    if destination_id:
        dest = _loc_by_pk(destination_id)
        if dest is None:
            raise LookupError(f"destination location {destination_id} not found")
    else:
        dest = require(staging_for(rack), "STAGING_LOCATION")

    item.move(dest, "SmartReel: picked", user)
    return {"slot": slot_num, "stock_id": item.pk, "moved_to": dest.pk}


def clear_slot(rack, slot_num: int, user, reason: str | None = None) -> dict:
    """Anomaly reconcile: move whatever is in the slot to the pulled bin."""
    slots = slot_map(rack)
    loc = slots.get(slot_num)
    if loc is None:
        raise LookupError(f"slot {slot_num} not found")
    item = stock_in(loc)
    if item is None:
        return {"slot": slot_num, "stock_id": None, "moved_to": None}

    dest = require(pulled_for(rack), "PULLED_LOCATION")
    item.move(dest, f"SmartReel: cleared ({reason or 'unspecified'})", user)
    return {"slot": slot_num, "stock_id": item.pk, "moved_to": dest.pk}


def locate_part(rack, wire_id: str) -> dict:
    part = part_by_wire_id(wire_id)
    if part is None:
        raise LookupError(f"part {wire_id} not found")
    slots = slot_map(rack)
    found = [
        n for n in sorted(slots)
        if (item := stock_in(slots[n])) and item.part_id == part.pk
    ]
    return {"part_id": wire_id, "slots": found}


# ---------------------------------------------------------------------------
# Locate requests (InvenTree "locate" button -> light a slot on the rack)
# ---------------------------------------------------------------------------
#
# InvenTree's LocateMixin offloads locate_stock_item / locate_stock_location
# to a background worker (group='plugin'); the plugin never talks to the rack
# directly, so a locate is recorded as a pending intent on the *rack's*
# metadata and the HMI consumes it. The HMI already polls GET /rack for
# reconciliation, so locates ride on that snapshot (`locates`) and the HMI
# acks consumed ids via POST /rack/locates/ack so they don't re-light forever.

LOCATE_QUEUE_KEY = "smartreel_locates"   # on a rack location: pending locates
LOCATE_QUEUE_CAP = 50


def slot_num_for_location(rack, location) -> int | None:
    """Slot number of `location` within `rack`, or None if it isn't a slot."""
    if location is None:
        return None
    for n, loc in slot_map(rack).items():
        if loc.pk == location.pk:
            return n
    return None


def rack_for_slot_location(location):
    """The (rack, slot_num) a slot location belongs to, or (None, None).

    A slot is a child of a rack tagged metadata['smartreel'] = {"slot": n};
    its parent is the rack location.
    """
    if location is None:
        return None, None
    meta = location.get_metadata(METADATA_KEY) or {}
    slot = meta.get("slot")
    if not isinstance(slot, int):
        return None, None
    rack = location.parent
    if not is_rack_location(rack):
        return None, None
    return rack, slot


def enqueue_locate(rack, slot_num: int, stock=None, part=None) -> dict:
    """Record a pending "light slot N" request on the rack. Idempotent on the
    (slot_num) target: a fresh locate for an already-queued slot refreshes it
    rather than stacking duplicates, so repeated button presses stay sane."""
    from stock.models import StockLocation

    # Atomic read-modify-write under a row lock: concurrent locate requests
    # must not read the same queue, compute the same next_id, and clobber each
    # other. Mirrors log_anomaly().
    with transaction.atomic():
        rack = StockLocation.objects.select_for_update().get(pk=rack.pk)
        prev = rack.get_metadata(LOCATE_QUEUE_KEY) or []
        # Drop any existing locate for this slot so repeated presses don't stack,
        # but keep ids monotonic across the whole (pre-dedupe) queue.
        queue = [e for e in prev if e.get("slot_num") != slot_num]
        next_id = max((e["id"] for e in prev), default=0) + 1
        entry = {
            "id": next_id,
            "slot_num": slot_num,
            "part": part_wire_id(part) if part is not None else None,
            "part_name": part.full_name if part is not None else None,
            "stock_id": stock.pk if stock is not None else None,
            "at": timezone.now().isoformat(timespec="seconds"),
        }
        queue.append(entry)
        rack.set_metadata(LOCATE_QUEUE_KEY, queue[-LOCATE_QUEUE_CAP:])
    logger.info("smartreel: rack %s locate slot %s (id=%s)", rack.pk, slot_num, next_id)
    return entry


def locate_requests(rack) -> list[dict]:
    """Pending locate requests for `rack` (oldest first)."""
    return list(rack.get_metadata(LOCATE_QUEUE_KEY) or [])


def ack_locates(rack, ids) -> dict:
    """Drop the given locate ids from the rack's queue (empty/None clears all).
    Idempotent: acking an unknown/already-acked id is a no-op. Returns the
    remaining queue."""
    prev = rack.get_metadata(LOCATE_QUEUE_KEY) or []
    if not ids:
        queue: list = []
    else:
        drop = {int(i) for i in ids}
        queue = [e for e in prev if e.get("id") not in drop]
    rack.set_metadata(LOCATE_QUEUE_KEY, queue)
    return {"locates": queue}


def locate_stock_location_pk(location_pk) -> dict:
    """Resolve a StockLocation pk to a rack slot and enqueue a locate.

    Called from the LocateMixin (background worker). If the location is a
    SmartReel slot, the locate is queued on its rack; if it's a rack itself,
    every occupied slot is lit. Locations outside any SmartReel rack are a
    no-op (another plugin / no rack owns them)."""
    loc = _loc_by_pk(location_pk)
    if loc is None:
        logger.warning("smartreel locate: location %s not found", location_pk)
        return {"located": False, "reason": "location not found"}

    rack, slot_num = rack_for_slot_location(loc)
    if rack is not None:
        item = stock_in(loc)
        enqueue_locate(rack, slot_num, stock=item,
                       part=item.part if item is not None else None)
        return {"located": True, "rack_id": rack.pk, "slots": [slot_num]}

    # The location may be a rack as a whole -> light all of its occupied slots.
    if is_rack_location(loc):
        lit = []
        for n, sloc in sorted(slot_map(loc).items()):
            item = stock_in(sloc)
            if item is not None:
                enqueue_locate(loc, n, stock=item, part=item.part)
                lit.append(n)
        return {"located": bool(lit), "rack_id": loc.pk, "slots": lit}

    return {"located": False, "reason": "location is not in a SmartReel rack"}


def locate_stock_item_pk(item_pk) -> dict:
    """Resolve a StockItem pk to its rack slot and enqueue a locate.

    Mirrors LocateMixin's default item->location behaviour, but resolves the
    rack/slot ourselves so we can carry the part/stock context into the queue.
    """
    from stock.models import StockItem

    try:
        item = StockItem.objects.get(pk=item_pk)
    except StockItem.DoesNotExist:
        logger.warning("smartreel locate: stock item %s not found", item_pk)
        return {"located": False, "reason": "stock item not found"}

    if not item.in_stock or item.location is None:
        return {"located": False, "reason": "stock item not in stock"}

    rack, slot_num = rack_for_slot_location(item.location)
    if rack is None:
        return {"located": False, "reason": "stock item is not in a SmartReel slot"}

    enqueue_locate(rack, slot_num, stock=item, part=item.part)
    return {"located": True, "rack_id": rack.pk, "slots": [slot_num]}


# ---------------------------------------------------------------------------
# Barcode resolution (rack only scopes the reported slot_num)
# ---------------------------------------------------------------------------

def resolve_barcode(rack, code: str) -> dict:
    """Resolve a scanned code via InvenTree's barcode plugin registry.

    Covers InvenTree-generated QR labels (JSON + INV-SI short form) and
    custom codes linked to items at receival (barcode-hash match). `rack`
    (may be None) only determines whether the reported slot_num is filled.
    """
    from plugin import PluginMixinEnum, registry
    from part.models import Part
    from stock.models import StockItem, StockLocation

    slots = slot_map(rack) if rack is not None else {}

    match = None
    for bplugin in registry.with_mixin(PluginMixinEnum.BARCODE):
        try:
            result = bplugin.scan(code)
        except Exception:
            continue
        if result and "error" not in result:
            match = result
            break

    if not match:
        return {"type": "unknown", "message": f"Unknown barcode: {code[:48]}"}

    def _pk(payload):
        if isinstance(payload, dict):
            return payload.get("pk")
        return getattr(payload, "pk", payload)

    if "stockitem" in match:
        try:
            item = StockItem.objects.get(pk=_pk(match["stockitem"]))
        except StockItem.DoesNotExist:
            return {"type": "unknown", "message": "Stock item no longer exists"}
        # render_stock reports slot_num relative to THIS rack, so the HMI can
        # reject a reel already housed here yet accept one from a different
        # SmartReel (review item 21). No cross-rack block is needed: a reel
        # loaded elsewhere transfers over, and that rack notices via its fast
        # occupancy poll (item 6).
        return {"type": "stockitem", "stock": render_stock(item, slots)}

    if "part" in match:
        try:
            part = Part.objects.get(pk=_pk(match["part"]))
        except Part.DoesNotExist:
            return {"type": "unknown", "message": "Part no longer exists"}
        return {"type": "part", "part": render_part(part)}

    if "stocklocation" in match:
        if StockLocation.objects.filter(pk=_pk(match["stocklocation"])).exists():
            return {"type": "location"}

    return {"type": "unknown", "message": f"Unsupported barcode target: {code[:48]}"}


# ---------------------------------------------------------------------------
# Pick jobs (stored in BuildOrder metadata, targeted at a specific rack)
# ---------------------------------------------------------------------------

def _job_builds():
    """Yield (build, job) for every SmartReel pick job.

    A build can now carry one job per rack under JOBS_KEY = {str(rack_pk): job}
    (review item 10b). A pre-multi-rack single job under METADATA_KEY is still
    honoured (migrate-on-read), so old jobs keep working.
    """
    from build.models import Build

    # metadata is a JSONField; cheap python-side filter keeps this portable.
    for build in Build.objects.exclude(metadata__isnull=True):
        meta = build.metadata or {}
        jobs = meta.get(JOBS_KEY)
        if jobs:
            for job in jobs.values():
                yield build, job
        else:
            legacy = meta.get(METADATA_KEY)
            if legacy and isinstance(legacy, dict) and legacy.get("items"):
                yield build, legacy


def _build_by_reference(reference: str):
    from build.models import Build

    return Build.objects.filter(reference=reference).first()


def job_status(job: dict) -> str:
    picked = [it["picked"] for it in job["items"]]
    if picked and all(picked):
        return "done"
    if any(picked):
        return "partial"
    return "pending"


def _located(part_pk: int, slots: dict[int, object]) -> list[int]:
    return [
        n for n in sorted(slots)
        if (item := stock_in(slots[n])) and item.part_id == part_pk
    ]


def _located_item(item_meta: dict, slots: dict[int, object]) -> list[int]:
    """Slots in `slots` that can fulfil a job item. When the item is pinned to
    a specific reel (stock_id, review item 10a) only that reel's slot lights;
    otherwise any in-rack reel of the part does (legacy / unpinned)."""
    stock_id = item_meta.get("stock_id")
    if stock_id:
        return [
            n for n in sorted(slots)
            if (item := stock_in(slots[n])) and item.pk == stock_id
        ]
    return _located(item_meta["part"], slots)


def _job_belongs_to(job: dict, rack) -> bool:
    """A job targets `rack` iff its stored rack pk matches. Jobs without a
    rack pk (e.g. created before multi-unit) belong to no rack — re-send
    them from the build panel to assign a rack."""
    return job.get("rack") == rack.pk


def render_job(build, job: dict, slots: dict[int, object]) -> dict:
    return {
        "id": build.reference,
        "build_id": build.pk,        # for the web panel; HMI ignores it
        "rack_id": job.get("rack") or 0,
        "name": job.get("name") or build.title or build.reference,
        "requested_at": job.get("requested_at", ""),
        "status": job_status(job),
        "destination_id": job.get("destination_id") or 0,
        "items": [
            {
                "idx": i,
                "part_id": it["part_id"],
                "part_name": it["part_name"],
                "qty": it["qty"],
                "picked": it["picked"],
                "located_slots": [] if it["picked"] else _located_item(it, slots),
            }
            for i, it in enumerate(job["items"])
        ],
    }


def list_jobs(rack) -> list[dict]:
    """Jobs targeted at `rack`, with located_slots computed in its slots."""
    slots = slot_map(rack)
    return [render_job(b, j, slots) for b, j in _job_builds() if _job_belongs_to(j, rack)]


def all_jobs() -> list[dict]:
    """Every job across all racks (web panel; located_slots within each job's rack)."""
    out = []
    for b, j in _job_builds():
        rack = _loc_by_pk(j.get("rack"))
        slots = slot_map(rack) if rack is not None else {}
        out.append(render_job(b, j, slots))
    return out


def _slot_index() -> dict[int, tuple]:
    """{slot_location_pk: (rack, slot_num)} across every provisioned rack."""
    index: dict[int, tuple] = {}
    for rack in list_rack_locations():
        for n, loc in slot_map(rack).items():
            index[loc.pk] = (rack, n)
    return index


def build_stock_options(build) -> list[dict]:
    """For each BOM line, the in-stock reels housed in any SmartReel rack —
    the candidate reels the operator picks from before sending (review item
    10a). A part with no rack-housed stock shows an empty candidate list."""
    from stock.models import StockItem

    index = _slot_index()
    loc_pks = list(index)
    out = []
    for line in build.build_lines.all().select_related("bom_item__sub_part"):
        part = line.bom_item.sub_part
        candidates = []
        if loc_pks:
            for item in StockItem.objects.filter(
                part=part, quantity__gt=0, location_id__in=loc_pks
            ).select_related("part"):
                rack, slot_num = index[item.location_id]
                candidates.append({
                    "stock_id": item.pk,
                    "qty": int(item.quantity),
                    "batch": item.batch or "",
                    "rack_id": rack.pk,
                    "rack_name": rack.pathstring or rack.name,
                    "slot_num": slot_num,
                })
        candidates.sort(key=lambda c: (c["rack_name"], c["slot_num"]))
        out.append({
            "part": part.pk,
            "part_id": part_wire_id(part),
            "part_name": part.full_name,
            "qty": int(line.quantity),
            "candidates": candidates,
        })
    return out


def create_jobs_from_build(build, user, stock_ids, destination_id: int | None = None) -> list[dict]:
    """Create one pick job per rack from the selected reels (review item 10).

    `stock_ids` are the StockItem pks the operator ticked in the panel. Each is
    grouped by the rack that physically holds it, and one job is created per
    rack containing just those reels (each item pinned to its stock_id). Returns
    the rendered jobs (one per rack). Replaces any existing SmartReel jobs on
    the build.
    """
    from stock.models import StockItem

    if not stock_ids:
        raise ValueError("no reels selected to pick")

    index = _slot_index()
    by_rack: dict[int, tuple] = {}    # rack_pk -> (rack, [(item, slot_num)])
    for sid in stock_ids:
        try:
            item = StockItem.objects.select_related("part").get(pk=sid)
        except StockItem.DoesNotExist:
            raise LookupError(f"stock item {sid} not found")
        info = index.get(item.location_id)
        if info is None:
            raise SlotConflict(f"stock item {sid} is not in a SmartReel slot")
        rack, slot_num = info
        by_rack.setdefault(rack.pk, (rack, []))[1].append((item, slot_num))

    jobs_meta: dict[str, dict] = {}
    rendered: list[tuple] = []
    for rack, items in by_rack.values():
        if destination_id:
            dest = _loc_by_pk(destination_id)
            if dest is None:
                raise LookupError(f"destination location {destination_id} not found")
        else:
            dest = require(staging_for(rack), "STAGING_LOCATION")
        job_items = [
            {
                "part": item.part_id,
                "part_id": part_wire_id(item.part),
                "part_name": item.part.full_name,
                "qty": int(item.quantity),
                "stock_id": item.pk,
                "picked": False,
                "picked_stock": None,
            }
            for item, _slot in items
        ]
        job = {
            "name": f"{build.part.name} x {int(build.quantity)}",
            "rack": rack.pk,
            "requested_at": timezone.now().isoformat(timespec="seconds"),
            "requested_by": getattr(user, "username", ""),
            "destination_id": dest.pk,
            "items": job_items,
        }
        jobs_meta[str(rack.pk)] = job
        rendered.append((rack, job))

    # Replace any prior SmartReel jobs on this build (both new + legacy keys).
    build.set_metadata(JOBS_KEY, jobs_meta)
    meta = build.metadata or {}
    if METADATA_KEY in meta:
        del meta[METADATA_KEY]
        build.metadata = meta
        build.save()
    return [render_job(build, j, slot_map(r)) for r, j in rendered]


def delete_job(reference: str) -> bool:
    """Remove all SmartReel pick jobs for a build (every rack + legacy)."""
    build = _build_by_reference(reference)
    if build is None:
        return False
    meta = build.metadata or {}
    if JOBS_KEY not in meta and METADATA_KEY not in meta:
        return False
    meta.pop(JOBS_KEY, None)
    meta.pop(METADATA_KEY, None)
    build.metadata = meta
    build.save()
    return True


def _build_job_for_rack(build, rack):
    """The (job, storage_key) for `rack` on `build`, or (None, None)."""
    meta = build.metadata or {}
    jobs = meta.get(JOBS_KEY)
    if jobs and str(rack.pk) in jobs:
        return jobs[str(rack.pk)], JOBS_KEY
    legacy = meta.get(METADATA_KEY)
    if legacy and isinstance(legacy, dict) and legacy.get("rack") == rack.pk:
        return legacy, METADATA_KEY
    return None, None


def _write_build_job(build, rack, job, key) -> None:
    if key == JOBS_KEY:
        jobs = build.get_metadata(JOBS_KEY) or {}
        jobs[str(rack.pk)] = job
        build.set_metadata(JOBS_KEY, jobs)
    else:
        build.set_metadata(METADATA_KEY, job)


def pick_job_item(rack, reference: str, idx: int, slot_num: int, user) -> dict:
    build = _build_by_reference(reference)
    job, key = _build_job_for_rack(build, rack) if build else (None, None)
    if not job:
        raise LookupError(f"job {reference} not found for this rack")
    if idx < 0 or idx >= len(job["items"]):
        raise LookupError(f"item idx {idx} out of bounds")
    item_meta = job["items"][idx]
    if item_meta["picked"]:
        raise SlotConflict(f"item {idx} already picked")

    slots = slot_map(rack)
    loc = slots.get(slot_num)
    if loc is None:
        raise LookupError(f"slot {slot_num} not found")
    stock = stock_in(loc)
    if stock is None:
        raise SlotConflict(f"slot {slot_num} is empty")
    if stock.part_id != item_meta["part"]:
        raise SlotConflict(
            f"slot {slot_num} holds {part_wire_id(stock.part)}, "
            f"item needs {item_meta['part_id']}"
        )
    # When the item is pinned to a specific reel, the slot must hold THAT reel
    # (review item 10a) -- not just any reel of the part.
    pinned = item_meta.get("stock_id")
    if pinned and stock.pk != pinned:
        raise SlotConflict(
            f"slot {slot_num} holds reel {stock.pk}, item needs reel {pinned}"
        )

    # Atomic: the stock move and the job's "picked" bookkeeping must commit or
    # roll back together. A crash between them would otherwise move stock while
    # leaving the item unpicked — an unrecoverable partial state that the op_id
    # idempotency cache can't repair (the stock is already gone on retry).
    with transaction.atomic():
        result = pick_slot(rack, slot_num, user, destination_id=job.get("destination_id"))
        item_meta["picked"] = True
        item_meta["picked_stock"] = result["stock_id"]
        _write_build_job(build, rack, job, key)

    rendered = render_job(build, job, slots)
    return {
        "item": rendered["items"][idx],
        "job_status": rendered["status"],
    }


# ---------------------------------------------------------------------------
# Anomaly log (kept per-rack on the rack location's metadata, capped)
# ---------------------------------------------------------------------------

ANOMALY_LOG_KEY = "smartreel_anomalies"
ANOMALY_LOG_CAP = 200


def log_anomaly(rack, kind: str, slot_num: int | None, detail: str) -> dict:
    from stock.models import StockLocation

    # Atomic read-modify-write: two concurrent requests must not read the same
    # log, compute the same next_id, and clobber each other. Re-fetch the rack
    # row under a row lock so the read and the write are serialized.
    with transaction.atomic():
        rack = StockLocation.objects.select_for_update().get(pk=rack.pk)
        log = rack.get_metadata(ANOMALY_LOG_KEY) or []
        next_id = (log[-1]["id"] + 1) if log else 1
        entry = {
            "id": next_id,
            "kind": kind,
            "slot_num": slot_num,
            "detail": (detail or "")[:200],
            "at": timezone.now().isoformat(timespec="seconds"),
        }
        log.append(entry)
        rack.set_metadata(ANOMALY_LOG_KEY, log[-ANOMALY_LOG_CAP:])
    logger.warning("smartreel anomaly: rack=%s %s slot=%s %s",
                   rack.pk, kind, slot_num, detail)
    return {"id": next_id, "logged": True}
