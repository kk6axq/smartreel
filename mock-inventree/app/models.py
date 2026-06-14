"""Pydantic request/response models. Field shapes match docs/hmi-plugin-api.md.

These are the wire format; the in-memory state in store.py uses plain dicts so
we can serialise them straight out without dataclass<->pydantic round-trips.
"""
from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field


# ---------- resource shapes ----------

class Part(BaseModel):
    id: str
    name: str
    pkg: str
    mfg: str


class StockItem(BaseModel):
    id: int
    part: Part
    qty: int
    batch: str
    barcode: str
    location_id: int
    # If currently sitting in a rack slot, the slot number; else None.
    slot_num: int | None = None


class RackSlot(BaseModel):
    slot: int           # physical position, 1..N
    location_id: int
    stock: StockItem | None = None   # null == logically empty


class PickJobItem(BaseModel):
    idx: int
    part_id: str
    part_name: str
    qty: int            # required qty, informational on the HMI
    picked: bool
    located_slots: list[int] = Field(default_factory=list)


JobStatus = Literal["pending", "partial", "done"]


class PickJob(BaseModel):
    id: str
    name: str
    requested_at: str
    status: JobStatus   # derived: nothing / some / all items picked
    destination_id: int
    items: list[PickJobItem]


class LocateRequest(BaseModel):
    """A pending "light slot N" request from the InvenTree locate button."""
    id: int
    slot_num: int
    part: str | None = None        # part wire id, when known
    part_name: str | None = None
    stock_id: int | None = None
    at: str                        # ISO-8601


class Anomaly(BaseModel):
    id: int
    kind: Literal["removed", "added", "divider"]
    slot_num: int | None = None
    detail: str = ""
    at: str   # ISO-8601


# ---------- request bodies ----------

class ResolveReq(BaseModel):
    code: str
    op_id: str | None = None


class AssignReq(BaseModel):
    stock_item_id: int
    op_id: str


class PickReq(BaseModel):
    op_id: str
    destination_id: int | None = None   # default: staging location


class ClearReq(BaseModel):
    op_id: str
    reason: str | None = None


class JobPickReq(BaseModel):
    slot_num: int
    op_id: str


class AnomalyReq(BaseModel):
    kind: Literal["removed", "added", "divider"]
    slot_num: int | None = None
    detail: str = ""
    op_id: str


class RegisterReq(BaseModel):
    n_slots: int
    op_id: str


class InjectErrorReq(BaseModel):
    path: str            # exact path prefix, e.g. "/api/v1/rack/slots"
    status: int
    count: int = 1


class LocateInjectReq(BaseModel):
    """Dev-only: simulate the InvenTree locate button. Supply exactly one."""
    slot: int | None = None
    stock_id: int | None = None
    part_id: str | None = None


# ---------- response wrappers ----------

class HealthResp(BaseModel):
    ok: bool
    server: str
    version: str
    time: str


class ResolveResp(BaseModel):
    type: Literal["stockitem", "part", "location", "unknown"]
    stock: StockItem | None = None
    part: Part | None = None
    message: str | None = None


class AssignResp(BaseModel):
    slot: int
    stock: StockItem


class PickResp(BaseModel):
    slot: int
    stock_id: int
    moved_to: int


class ClearResp(BaseModel):
    slot: int
    stock_id: int | None = None    # None when the slot was already empty
    moved_to: int | None = None


class RackResp(BaseModel):
    location_id: int
    n_slots: int
    slots: list[RackSlot]
    pickjobs_available: int
    # Pending locate requests (InvenTree locate button); HMI lights these and
    # acks the ids via POST /rack/locates/ack.
    locates: list[LocateRequest] = Field(default_factory=list)


class LocateAckReq(BaseModel):
    # Locate ids to drop; empty / omitted clears the whole queue.
    ids: list[int] | None = None


class LocateAckResp(BaseModel):
    locates: list[LocateRequest] = Field(default_factory=list)


class PickJobsResp(BaseModel):
    jobs: list[PickJob]


class JobPickResp(BaseModel):
    item: PickJobItem
    job_status: JobStatus


class AnomalyResp(BaseModel):
    id: int
    logged: bool = True


class RegisterResp(BaseModel):
    location_id: int
    slot_locations: list[dict[str, int]]   # [{slot, location_id}, ...]


class LocateResp(BaseModel):
    part_id: str
    slots: list[int]
