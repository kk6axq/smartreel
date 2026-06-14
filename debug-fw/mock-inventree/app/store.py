"""In-memory state for the mock plugin server.

Everything lives in `state` and is guarded by `_lock` (re-entrant so ops can
nest helper reads under a mutating section). FastAPI runs sync endpoints in a
thread pool, so a plain `threading.RLock` is the right primitive.

Renderers translate the internal dict shapes into the wire shapes from
`models.py`. The wire models accept dicts directly, so we return plain dicts
and FastAPI validates on the way out.
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Any

from .config import settings


# ---------- state ----------

@dataclass
class State:
    parts: dict[str, dict] = field(default_factory=dict)            # part_id -> Part
    stock_items: dict[int, dict] = field(default_factory=dict)      # si_id -> StockItem
    slots: list[dict] = field(default_factory=list)                 # idx == slot_num - 1
    pick_jobs: dict[str, dict] = field(default_factory=dict)        # job_id -> PickJob
    anomalies: list[dict] = field(default_factory=list)
    locates: list[dict] = field(default_factory=list)              # pending locate requests
    # op_id -> (expires_at, response_dict). Idempotency for mutating endpoints.
    op_cache: dict[str, tuple[float, Any]] = field(default_factory=dict)
    # [{path: str, status: int, count: int}], counted-down by middleware.
    error_injections: list[dict] = field(default_factory=list)
    next_stock_id: int = 1000
    next_anomaly_id: int = 1
    next_locate_id: int = 1


state = State()
_lock = threading.RLock()


def lock() -> threading.RLock:
    return _lock


# ---------- lookups ----------

def slot_at(slot_num: int) -> dict | None:
    if slot_num < 1 or slot_num > settings.n_slots:
        return None
    return state.slots[slot_num - 1]


def slot_location_id(slot_num: int) -> int:
    """Synthetic InvenTree sub-location id for slot N.

    Encoded as `rack_id * 100 + slot_num`; e.g. rack 42, slot 3 -> 4203. The
    real plugin will keep its own mapping table; this is just a deterministic
    placeholder so the HMI can see distinct location ids in responses.
    """
    return settings.rack_location_id * 100 + slot_num


def find_stock_by_barcode(code: str) -> dict | None:
    for si in state.stock_items.values():
        if si["barcode"] == code:
            return si
    return None


def find_stock_slot(si_id: int) -> int | None:
    """slot_num where stock item `si_id` currently sits, or None."""
    for s in state.slots:
        if s.get("stock_id") == si_id:
            return s["slot"]
    return None


def located_slots_for(part_id: str) -> list[int]:
    """Slot numbers currently holding any stock of `part_id`."""
    out: list[int] = []
    for slot in state.slots:
        sid = slot.get("stock_id")
        if not sid:
            continue
        si = state.stock_items.get(sid)
        if si and si["part_id"] == part_id:
            out.append(slot["slot"])
    return out


# ---------- locate queue (InvenTree locate button -> light a slot) ----------

def enqueue_locate(slot_num: int) -> dict:
    """Queue a "light slot N" request, mirroring the real plugin: a fresh
    locate for an already-queued slot refreshes it instead of stacking. Caller
    holds the lock."""
    import time as _time

    state.locates = [e for e in state.locates if e["slot_num"] != slot_num]
    slot = slot_at(slot_num)
    sid = slot.get("stock_id") if slot else None
    si = state.stock_items.get(sid) if sid else None
    part = state.parts.get(si["part_id"]) if si else None
    entry = {
        "id": state.next_locate_id,
        "slot_num": slot_num,
        "part": part["id"] if part else None,
        "part_name": part["name"] if part else None,
        "stock_id": sid,
        "at": _time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    state.next_locate_id += 1
    state.locates.append(entry)
    return entry


def ack_locates(ids: list[int] | None) -> list[dict]:
    """Drop the given locate ids (empty/None clears all). Caller holds the lock."""
    if not ids:
        state.locates = []
    else:
        drop = set(ids)
        state.locates = [e for e in state.locates if e["id"] not in drop]
    return state.locates


# ---------- renderers (internal dict -> wire dict) ----------

def render_part(p: dict) -> dict:
    return {"id": p["id"], "name": p["name"], "pkg": p["pkg"], "mfg": p["mfg"]}


def render_stock(si: dict) -> dict:
    return {
        "id": si["id"],
        "part": render_part(state.parts[si["part_id"]]),
        "qty": si["qty"],
        "batch": si["batch"],
        "barcode": si["barcode"],
        "location_id": si["location_id"],
        "slot_num": find_stock_slot(si["id"]),
    }


def render_slot(slot: dict) -> dict:
    stock = None
    sid = slot.get("stock_id")
    if sid is not None:
        si = state.stock_items.get(sid)
        if si is not None:
            stock = render_stock(si)
    return {
        "slot": slot["slot"],
        "location_id": slot["location_id"],
        "stock": stock,
    }


def render_pick_item(it: dict, part_name: str | None = None) -> dict:
    pid = it["part_id"]
    name = part_name or (state.parts.get(pid, {}).get("name") or pid)
    return {
        "idx": it["idx"],
        "part_id": pid,
        "part_name": name,
        "qty": it["qty"],
        "picked": it["picked"],
        "located_slots": located_slots_for(pid),
    }


def job_status(job: dict) -> str:
    """Derived per docs/hmi-plugin-api.md: pending / partial / done."""
    picked = [it["picked"] for it in job["items"]]
    if all(picked):
        return "done"
    if any(picked):
        return "partial"
    return "pending"


def render_pick_job(job: dict) -> dict:
    return {
        "id": job["id"],
        "name": job["name"],
        "requested_at": job["requested_at"],
        "status": job_status(job),
        "destination_id": job["destination_id"],
        "items": [render_pick_item(it) for it in job["items"]],
    }


# ---------- whole-reel pick ----------

def pick_reel(slot: dict, destination_id: int) -> dict:
    """Transfer the slot's StockItem to `destination_id` and free the slot.

    Caller holds the lock and has verified the slot is occupied. Returns the
    stock item dict (now homed at the destination).
    """
    si = state.stock_items[slot["stock_id"]]
    si["location_id"] = destination_id
    slot["state"] = "EMPTY"
    slot["stock_id"] = None
    return si


# ---------- idempotency cache ----------

def op_lookup(op_id: str | None) -> Any | None:
    if not op_id:
        return None
    _gc_op_cache()
    entry = state.op_cache.get(op_id)
    return None if entry is None else entry[1]


def op_remember(op_id: str | None, resp: Any) -> None:
    if not op_id:
        return
    state.op_cache[op_id] = (time.time() + settings.op_cache_ttl_s, resp)


def _gc_op_cache() -> None:
    now = time.time()
    expired = [k for k, (exp, _) in state.op_cache.items() if exp < now]
    for k in expired:
        state.op_cache.pop(k, None)


# ---------- error injection ----------

def consume_error_injection(path: str) -> int | None:
    """If a `/_dev/inject_error` entry matches `path`, decrement and return its
    status code. Used by the middleware in main.py.
    """
    for inj in list(state.error_injections):
        if path.startswith(inj["path"]) and inj["count"] > 0:
            inj["count"] -= 1
            if inj["count"] <= 0:
                state.error_injections.remove(inj)
            return inj["status"]
    return None
