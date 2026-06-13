"""Read-side helpers for the catalogue.

`locate` powers a "where is this part?" flow independent of pick jobs.
"""
from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException

from .. import store
from ..auth import require_token
from ..models import LocateResp


router = APIRouter(
    prefix="/api/v1/parts",
    tags=["parts"],
    dependencies=[Depends(require_token)],
)


@router.get("/locate", response_model=LocateResp)
def locate(part_id: str) -> LocateResp:
    with store.lock():
        part = store.state.parts.get(part_id)
        if part is None:
            raise HTTPException(status_code=404, detail=f"part {part_id} not found")
        return LocateResp(part_id=part["id"], slots=store.located_slots_for(part_id))
