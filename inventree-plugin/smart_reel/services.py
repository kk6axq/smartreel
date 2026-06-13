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
from django.utils import timezone

logger = logging.getLogger("inventree")

METADATA_KEY = "smartreel"          # on slot locations: {"slot": n}
                                    # on rack locations: {"is_rack": true, "staging", "pulled"}
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
    for n in range(1, n_slots + 1):
        if n in existing:
            continue
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
    }


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
    from build.models import Build

    # metadata is a JSONField; cheap python-side filter keeps this portable.
    for build in Build.objects.exclude(metadata__isnull=True):
        job = (build.metadata or {}).get(METADATA_KEY)
        if job:
            yield build, job


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
                "located_slots": [] if it["picked"] else _located(it["part"], slots),
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


def create_job_from_build(build, user, rack, destination_id: int | None = None) -> dict:
    """Create (or replace) the SmartReel pick job for a build order, targeted
    at `rack`. One item per build line; parts not in the rack still get an
    item (located_slots comes back empty → HMI shows them unfulfillable).
    """
    if rack is None:
        raise ConfigError("no SmartReel rack selected for this job")

    if destination_id:
        dest = _loc_by_pk(destination_id)
        if dest is None:
            raise LookupError(f"destination location {destination_id} not found")
    else:
        dest = require(staging_for(rack), "STAGING_LOCATION")

    items = []
    for line in build.build_lines.all().select_related("bom_item__sub_part"):
        part = line.bom_item.sub_part
        items.append({
            "part": part.pk,
            "part_id": part_wire_id(part),
            "part_name": part.full_name,
            "qty": int(line.quantity),
            "picked": False,
            "picked_stock": None,
        })
    if not items:
        raise ValueError("build order has no BOM lines to pick")

    job = {
        "name": f"{build.part.name} x {int(build.quantity)}",
        "rack": rack.pk,
        "requested_at": timezone.now().isoformat(timespec="seconds"),
        "requested_by": getattr(user, "username", ""),
        "destination_id": dest.pk,
        "items": items,
    }
    build.set_metadata(METADATA_KEY, job)
    return render_job(build, job, slot_map(rack))


def delete_job(reference: str) -> bool:
    build = _build_by_reference(reference)
    if build is None:
        return False
    meta = build.metadata or {}
    if METADATA_KEY not in meta:
        return False
    del meta[METADATA_KEY]
    build.metadata = meta
    build.save()
    return True


def pick_job_item(rack, reference: str, idx: int, slot_num: int, user) -> dict:
    build = _build_by_reference(reference)
    job = (build.metadata or {}).get(METADATA_KEY) if build else None
    if not job:
        raise LookupError(f"job {reference} not found")
    if not _job_belongs_to(job, rack):
        raise SlotConflict(f"job {reference} is not assigned to this rack")
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

    result = pick_slot(rack, slot_num, user, destination_id=job.get("destination_id"))

    item_meta["picked"] = True
    item_meta["picked_stock"] = result["stock_id"]
    build.set_metadata(METADATA_KEY, job)

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
