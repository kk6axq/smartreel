"""Anomaly intake. The HMI pushes detected unscanned activity here."""
from __future__ import annotations

import datetime as dt

from fastapi import APIRouter, Depends

from .. import store
from ..auth import require_token
from ..models import AnomalyReq, AnomalyResp


router = APIRouter(
    prefix="/api/v1/anomaly",
    tags=["anomaly"],
    dependencies=[Depends(require_token)],
)


@router.post("", response_model=AnomalyResp)
def log(req: AnomalyReq) -> AnomalyResp:
    cached = store.op_lookup(req.op_id)
    if cached is not None:
        return AnomalyResp(**cached)

    with store.lock():
        aid = store.state.next_anomaly_id
        store.state.next_anomaly_id += 1
        store.state.anomalies.append({
            "id": aid,
            "kind": req.kind,
            "slot_num": req.slot_num,
            "detail": req.detail,
            "at": dt.datetime.now(dt.timezone.utc).isoformat(),
        })
        resp = {"id": aid, "logged": True}

    store.op_remember(req.op_id, resp)
    return AnomalyResp(**resp)
