"""Pick-job listing + per-item pick application.

Jobs model user story 4: an item is one part; picking it means removing one
reel of that part from a lit slot, which whole-reel-transfers that reel to
the job's destination. Job status is derived (pending/partial/done) — the
HMI's cancel button is purely local, so there is no cancel endpoint.
"""
from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException

from .. import store
from ..auth import require_token
from ..models import JobPickReq, JobPickResp, PickJobsResp


router = APIRouter(
    prefix="/api/v1/pickjobs",
    tags=["pickjobs"],
    dependencies=[Depends(require_token)],
)


@router.get("", response_model=PickJobsResp)
def list_jobs() -> PickJobsResp:
    with store.lock():
        jobs = [store.render_pick_job(j) for j in store.state.pick_jobs.values()]
    return PickJobsResp(jobs=jobs)


@router.post("/{job_id}/items/{idx}/pick", response_model=JobPickResp)
def pick_item(job_id: str, idx: int, req: JobPickReq) -> JobPickResp:
    """Pick job item `idx` by removing the reel in `req.slot_num`.

    Verifies the slot holds the item's part, transfers that reel to the
    job's destination (whole-reel), and marks the item picked.
    """
    cached = store.op_lookup(req.op_id)
    if cached is not None:
        return JobPickResp(**cached)

    with store.lock():
        job = store.state.pick_jobs.get(job_id)
        if job is None:
            raise HTTPException(status_code=404, detail=f"job {job_id} not found")
        if idx < 0 or idx >= len(job["items"]):
            raise HTTPException(status_code=404, detail=f"item idx {idx} oob")
        item = job["items"][idx]
        if item["picked"]:
            raise HTTPException(status_code=409, detail=f"item {idx} already picked")

        slot = store.slot_at(req.slot_num)
        if slot is None:
            raise HTTPException(status_code=404, detail=f"slot {req.slot_num} not found")
        if slot["state"] != "OCCUPIED" or slot.get("stock_id") is None:
            raise HTTPException(status_code=409, detail=f"slot {req.slot_num} is empty")
        si = store.state.stock_items[slot["stock_id"]]
        if si["part_id"] != item["part_id"]:
            raise HTTPException(
                status_code=409,
                detail=f"slot {req.slot_num} holds {si['part_id']}, item needs {item['part_id']}",
            )

        store.pick_reel(slot, job["destination_id"])
        item["picked"] = True

        resp = {
            "item": store.render_pick_item(item),
            "job_status": store.job_status(job),
        }

    store.op_remember(req.op_id, resp)
    return JobPickResp(**resp)
