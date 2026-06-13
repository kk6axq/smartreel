"""Domain logic for the SmartReel plugin.

Everything that touches InvenTree models lives here; api.py stays a thin
HTTP layer. The wire shapes produced here are specified in
docs/hmi-plugin-api.md (SmartReel repo) — keep them in sync with the mock.

Conventions:
- "slot" is a physical position 1..N, modelled as a StockLocation child of
  the configured rack location. Slot locations carry
  metadata['smartreel'] = {'slot': n} so renames don't break the mapping.
- Wire part ids are the part's IPN when set, else its pk as a string.
- Picks are whole-reel transfers (StockItem.move), never quantity math.
"""
from __future__ import annotations

import logging

from django.core.cache import cache
from django.utils import timezone

logger = logging.getLogger("inventree")

METADATA_KEY = "smartreel"
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


def _location(setting_key: str):
    """StockLocation referenced by a plugin setting, or None."""
    from stock.models import StockLocation

    pk = get_plugin().get_setting(setting_key)
    if not pk:
        return None
    try:
        return StockLocation.objects.get(pk=int(pk))
    except (StockLocation.DoesNotExist, ValueError, TypeError):
        return None


def rack_location():
    return _location("RACK_LOCATION")


def staging_location():
    return _location("STAGING_LOCATION")


def pulled_location():
    return _location("PULLED_LOCATION")


class ConfigError(Exception):
    """A required plugin setting is missing/invalid. Maps to HTTP 409."""


def require(loc, what: str):
    if loc is None:
        raise ConfigError(
            f"SmartReel plugin setting '{what}' is not configured "
            f"(InvenTree → Settings → Plugins → Smart Reel)"
        )
    return loc


# ---------------------------------------------------------------------------
# Slot locations
# ---------------------------------------------------------------------------

def slot_map() -> dict[int, object]:
    """{slot_num: StockLocation} for every registered slot."""
    from stock.models import StockLocation

    rack = rack_location()
    if rack is None:
        return {}
    out: dict[int, object] = {}
    for loc in StockLocation.objects.filter(parent=rack):
        meta = loc.get_metadata(METADATA_KEY) or {}
        slot = meta.get("slot")
        if isinstance(slot, int):
            out[slot] = loc
    return out


def ensure_slots(n_slots: int, user=None) -> dict[int, object]:
    """Create missing slot sub-locations 1..n_slots. Returns the full map."""
    from stock.models import StockLocation

    rack = require(rack_location(), "RACK_LOCATION")
    existing = slot_map()
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
        logger.info("smartreel: created slot location %s (pk=%s)", n, loc.pk)
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


def render_stock(item, slots: dict[int, object] | None = None) -> dict:
    if slots is None:
        slots = slot_map()
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
# Rack snapshot + slot ops
# ---------------------------------------------------------------------------

def rack_snapshot() -> dict:
    rack = require(rack_location(), "RACK_LOCATION")
    slots = slot_map()
    out = []
    for n in sorted(slots):
        loc = slots[n]
        item = stock_in(loc)
        out.append({
            "slot": n,
            "location_id": loc.pk,
            "stock": render_stock(item, slots) if item else None,
        })
    jobs = [j for j in list_jobs() if j["status"] != "done"]
    return {
        "location_id": rack.pk,
        "n_slots": len(out),
        "slots": out,
        "pickjobs_available": len(jobs),
    }


def assign_slot(slot_num: int, stock_item_id: int, user) -> dict:
    from stock.models import StockItem

    slots = slot_map()
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


class SlotConflict(Exception):
    """Maps to HTTP 409."""


def pick_slot(slot_num: int, user, destination_id: int | None = None) -> dict:
    """Whole-reel pick: transfer the slot's StockItem to the destination."""
    from stock.models import StockLocation

    slots = slot_map()
    loc = slots.get(slot_num)
    if loc is None:
        raise LookupError(f"slot {slot_num} not found")
    item = stock_in(loc)
    if item is None:
        raise SlotConflict(f"slot {slot_num} is empty")

    if destination_id:
        try:
            dest = StockLocation.objects.get(pk=destination_id)
        except StockLocation.DoesNotExist:
            raise LookupError(f"destination location {destination_id} not found")
    else:
        dest = require(staging_location(), "STAGING_LOCATION")

    item.move(dest, "SmartReel: picked", user)
    return {"slot": slot_num, "stock_id": item.pk, "moved_to": dest.pk}


def clear_slot(slot_num: int, user, reason: str | None = None) -> dict:
    """Anomaly reconcile: move whatever is in the slot to the pulled bin."""
    slots = slot_map()
    loc = slots.get(slot_num)
    if loc is None:
        raise LookupError(f"slot {slot_num} not found")
    item = stock_in(loc)
    if item is None:
        return {"slot": slot_num, "stock_id": None, "moved_to": None}

    dest = require(pulled_location(), "PULLED_LOCATION")
    item.move(dest, f"SmartReel: cleared ({reason or 'unspecified'})", user)
    return {"slot": slot_num, "stock_id": item.pk, "moved_to": dest.pk}


def locate_part(wire_id: str) -> dict:
    part = part_by_wire_id(wire_id)
    if part is None:
        raise LookupError(f"part {wire_id} not found")
    slots = slot_map()
    found = [
        n for n in sorted(slots)
        if (item := stock_in(slots[n])) and item.part_id == part.pk
    ]
    return {"part_id": wire_id, "slots": found}


# ---------------------------------------------------------------------------
# Barcode resolution
# ---------------------------------------------------------------------------

def resolve_barcode(code: str) -> dict:
    """Resolve a scanned code via InvenTree's barcode plugin registry.

    Covers InvenTree-generated QR labels (JSON + INV-SI short form) and
    custom codes linked to items at receival (barcode-hash match).
    """
    from plugin import PluginMixinEnum, registry
    from part.models import Part
    from stock.models import StockItem, StockLocation

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
        return {"type": "stockitem", "stock": render_stock(item)}

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
# Pick jobs (stored in BuildOrder metadata)
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


def render_job(build, job: dict, slots: dict[int, object] | None = None) -> dict:
    if slots is None:
        slots = slot_map()
    return {
        "id": build.reference,
        "build_id": build.pk,   # for the web panel; HMI ignores it
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


def list_jobs() -> list[dict]:
    slots = slot_map()
    return [render_job(b, j, slots) for b, j in _job_builds()]


def create_job_from_build(build, user, destination_id: int | None = None) -> dict:
    """Create (or replace) the SmartReel pick job for a build order.

    One item per build line; parts not currently in the rack still get an
    item (located_slots comes back empty → HMI shows them unfulfillable).
    """
    dest = None
    if destination_id:
        from stock.models import StockLocation

        dest = StockLocation.objects.filter(pk=destination_id).first()
        if dest is None:
            raise LookupError(f"destination location {destination_id} not found")
    else:
        dest = require(staging_location(), "STAGING_LOCATION")

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
        "requested_at": timezone.now().isoformat(timespec="seconds"),
        "requested_by": getattr(user, "username", ""),
        "destination_id": dest.pk,
        "items": items,
    }
    build.set_metadata(METADATA_KEY, job)
    return render_job(build, job)


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


def pick_job_item(reference: str, idx: int, slot_num: int, user) -> dict:
    build = _build_by_reference(reference)
    job = (build.metadata or {}).get(METADATA_KEY) if build else None
    if not job:
        raise LookupError(f"job {reference} not found")
    if idx < 0 or idx >= len(job["items"]):
        raise LookupError(f"item idx {idx} out of bounds")
    item_meta = job["items"][idx]
    if item_meta["picked"]:
        raise SlotConflict(f"item {idx} already picked")

    slots = slot_map()
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

    result = pick_slot(slot_num, user, destination_id=job.get("destination_id"))

    item_meta["picked"] = True
    item_meta["picked_stock"] = result["stock_id"]
    build.set_metadata(METADATA_KEY, job)

    rendered = render_job(build, job, slots)
    return {
        "item": rendered["items"][idx],
        "job_status": rendered["status"],
    }


# ---------------------------------------------------------------------------
# Anomaly log (kept on the rack location's metadata, capped)
# ---------------------------------------------------------------------------

ANOMALY_LOG_KEY = "smartreel_anomalies"
ANOMALY_LOG_CAP = 200


def log_anomaly(kind: str, slot_num: int | None, detail: str) -> dict:
    rack = require(rack_location(), "RACK_LOCATION")
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
    logger.warning("smartreel anomaly: %s slot=%s %s", kind, slot_num, detail)
    return {"id": next_id, "logged": True}
