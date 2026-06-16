"""Rack snapshot + slot mutations.

Slot ops are the bulk of the contract: assign (load), pick (whole-reel
transfer to staging), clear (anomaly reconcile). Every mutation goes through
op_id-keyed idempotency so a retried request from a flaky-WiFi HMI is safe.
"""
from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException

from .. import store
from ..auth import require_token
from ..config import settings
from ..models import (
    AssignReq,
    AssignResp,
    ClearReq,
    ClearResp,
    LocateAckReq,
    LocateAckResp,
    PickReq,
    PickResp,
    RackResp,
    RegisterReq,
    RegisterResp,
)


router = APIRouter(
    prefix="/api/v1/rack",
    tags=["rack"],
    dependencies=[Depends(require_token)],
)


# ---------- snapshot ----------

@router.get("", response_model=RackResp)
def get_rack() -> RackResp:
    with store.lock():
        slots = [store.render_slot(s) for s in store.state.slots]
        jobs_avail = sum(
            1 for j in store.state.pick_jobs.values()
            if store.job_status(j) != "done"
        )
        return RackResp(
            location_id=settings.rack_location_id,
            n_slots=len(slots),
            slots=slots,
            pickjobs_available=jobs_avail,
            locates=list(store.state.locates),
        )


@router.get("/occupancy")
def get_occupancy() -> dict:
    """Cheap occupancy fingerprint for the HMI's fast-poll change detector
    (review item 6). Opaque hash; the HMI only compares it for equality."""
    import hashlib
    import json

    with store.lock():
        rows = sorted(
            (s["slot"], s.get("stock_id"))
            for s in store.state.slots if s.get("stock_id") is not None
        )
        locs = store.state.locates
        loc_sig = [max((loc["id"] for loc in locs), default=0), len(locs)]
    payload = json.dumps([rows, loc_sig], sort_keys=True, default=str).encode()
    return {"rev": hashlib.sha1(payload).hexdigest()[:16]}


@router.post("/locates/ack", response_model=LocateAckResp)
def ack_locates(req: LocateAckReq) -> LocateAckResp:
    """Drop consumed locate requests (InvenTree locate button). Body
    {"ids": [...]} acks specific ids; {} / {"ids": []} clears the queue.
    Idempotent: acking an unknown id is a no-op."""
    with store.lock():
        remaining = store.ack_locates(req.ids)
        return LocateAckResp(locates=list(remaining))


@router.post("/register", response_model=RegisterResp)
def register(req: RegisterReq) -> RegisterResp:
    """Ensure the rack's slot sub-locations exist. Idempotent; grows the rack
    when the HMI reports more physical slots than we currently model."""
    cached = store.op_lookup(req.op_id)
    if cached is not None:
        return RegisterResp(**cached)

    if req.n_slots < 1 or req.n_slots > 512:
        raise HTTPException(status_code=400, detail=f"implausible n_slots {req.n_slots}")

    with store.lock():
        from ..seed import _empty_slot  # local import to avoid a cycle at boot
        while len(store.state.slots) < req.n_slots:
            store.state.slots.append(_empty_slot(len(store.state.slots) + 1))
        resp = {
            "location_id": settings.rack_location_id,
            "slot_locations": [
                {"slot": s["slot"], "location_id": s["location_id"]}
                for s in store.state.slots
            ],
        }
    store.op_remember(req.op_id, resp)
    return RegisterResp(**resp)


# ---------- slot mutations ----------

@router.post("/slots/{slot_num}/assign", response_model=AssignResp)
def assign(slot_num: int, req: AssignReq) -> AssignResp:
    cached = store.op_lookup(req.op_id)
    if cached is not None:
        return AssignResp(**cached)

    with store.lock():
        slot = store.slot_at(slot_num)
        if slot is None:
            raise HTTPException(status_code=404, detail=f"slot {slot_num} not found")
        si = store.state.stock_items.get(req.stock_item_id)
        if si is None:
            raise HTTPException(status_code=404, detail=f"stock item {req.stock_item_id} not found")
        if slot["state"] == "OCCUPIED" and slot.get("stock_id") != req.stock_item_id:
            raise HTTPException(
                status_code=409,
                detail=f"slot {slot_num} already holds stock item {slot.get('stock_id')}",
            )

        # If the stock item was sitting in a different slot, vacate it.
        prev_slot_num = store.find_stock_slot(req.stock_item_id)
        if prev_slot_num is not None and prev_slot_num != slot_num:
            prev = store.slot_at(prev_slot_num)
            if prev is not None:
                prev["state"] = "EMPTY"
                prev["stock_id"] = None

        slot["state"] = "OCCUPIED"
        slot["stock_id"] = req.stock_item_id
        si["location_id"] = slot["location_id"]

        resp = {"slot": slot_num, "stock": store.render_stock(si)}

    store.op_remember(req.op_id, resp)
    return AssignResp(**resp)


@router.post("/slots/{slot_num}/pick", response_model=PickResp)
def pick(slot_num: int, req: PickReq) -> PickResp:
    """Whole-reel pick: transfer the slot's StockItem to the destination
    (default: staging) and free the slot. No quantity math — the reel
    physically left the rack (user story 3)."""
    cached = store.op_lookup(req.op_id)
    if cached is not None:
        return PickResp(**cached)

    with store.lock():
        slot = store.slot_at(slot_num)
        if slot is None:
            raise HTTPException(status_code=404, detail=f"slot {slot_num} not found")
        if slot["state"] != "OCCUPIED" or slot.get("stock_id") is None:
            raise HTTPException(status_code=409, detail=f"slot {slot_num} is empty")

        dest = req.destination_id or settings.staging_location_id
        si = store.pick_reel(slot, dest)
        resp = {"slot": slot_num, "stock_id": si["id"], "moved_to": dest}

    store.op_remember(req.op_id, resp)
    return PickResp(**resp)


@router.post("/slots/{slot_num}/clear", response_model=ClearResp)
def clear(slot_num: int, req: ClearReq) -> ClearResp:
    """Move whatever is in the slot to the 'pulled / unsorted' location.

    Used when the HMI detects a physical removal without a scan (anomaly).
    The stock item is preserved so it isn't silently lost. Idempotent goal
    state: 2xx even if the slot was already logically empty.
    """
    cached = store.op_lookup(req.op_id)
    if cached is not None:
        return ClearResp(**cached)

    with store.lock():
        slot = store.slot_at(slot_num)
        if slot is None:
            raise HTTPException(status_code=404, detail=f"slot {slot_num} not found")
        sid = slot.get("stock_id")
        if sid is not None and sid in store.state.stock_items:
            store.pick_reel(slot, settings.pulled_location_id)
            resp = {"slot": slot_num, "stock_id": sid,
                    "moved_to": settings.pulled_location_id}
        else:
            slot["state"] = "EMPTY"
            slot["stock_id"] = None
            resp = {"slot": slot_num, "stock_id": None, "moved_to": None}

    store.op_remember(req.op_id, resp)
    return ClearResp(**resp)
