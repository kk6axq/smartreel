"""Barcode -> resource resolution. Drives every QR scan in the HMI."""
from __future__ import annotations

from fastapi import APIRouter, Depends

from .. import store
from ..auth import require_token
from ..models import ResolveReq, ResolveResp


router = APIRouter(
    prefix="/api/v1/barcode",
    tags=["barcode"],
    dependencies=[Depends(require_token)],
)


@router.post("/resolve", response_model=ResolveResp)
def resolve(req: ResolveReq) -> ResolveResp:
    cached = store.op_lookup(req.op_id)
    if cached is not None:
        return ResolveResp(**cached)

    with store.lock():
        si = store.find_stock_by_barcode(req.code)
        if si is not None:
            resp: dict = {"type": "stockitem", "stock": store.render_stock(si)}
        elif req.code in store.state.parts:
            # Raw part-number QR (rare in practice, useful for manual entry).
            resp = {"type": "part", "part": store.render_part(store.state.parts[req.code])}
        else:
            resp = {"type": "unknown", "message": f"Unknown barcode: {req.code}"}

    store.op_remember(req.op_id, resp)
    return ResolveResp(**resp)
