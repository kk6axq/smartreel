"""Dev / test helpers. Token-gated; expected to disappear in the real plugin.

These let us script HMI integration tests: hard-reset the server state,
inspect anomalies the device pushed, peek at the idempotency cache, and
inject HTTP errors on chosen paths to exercise failure UI.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os

from fastapi import APIRouter, Depends, HTTPException, Request

from .. import store
from ..auth import require_token
from ..models import InjectErrorReq, LocateInjectReq
from ..seed import reseed


router = APIRouter(
    prefix="/api/v1/_dev",
    tags=["dev"],
    dependencies=[Depends(require_token)],
)


@router.post("/reset")
def reset() -> dict:
    reseed()
    return {"ok": True}


@router.get("/anomalies")
def list_anomalies() -> dict:
    with store.lock():
        return {"anomalies": list(store.state.anomalies)}


@router.get("/ops")
def list_ops() -> dict:
    with store.lock():
        return {"count": len(store.state.op_cache), "op_ids": list(store.state.op_cache.keys())}


@router.get("/provision")
def provision(request: Request) -> dict:
    """SRPROV1 provisioning payload + QR for the dashboard.

    Mirrors what the real plugin's settings panel shows: the HMI scans the
    QR instead of typing URL/token (docs/hmi-plugin-api.md, Provisioning).
    The base URL is whatever host the browser used to reach us — open the
    dashboard via the LAN IP so the payload is reachable from the HMI.
    """
    from ..config import settings

    base = str(request.base_url).rstrip("/")
    payload_obj: dict = {"u": base, "t": settings.token}
    fp = _cert_fingerprint()
    if fp:
        payload_obj["f"] = fp
    payload = "SRPROV1:" + json.dumps(payload_obj, separators=(",", ":"))

    out: dict = {"payload": payload}
    try:
        import io
        import segno
        buf = io.BytesIO()   # segno emits SVG as bytes
        segno.make(payload, error="m").save(buf, kind="svg", scale=4, border=2)
        out["svg"] = buf.getvalue().decode("utf-8")
    except ImportError:
        out["svg"] = None   # pip install segno (in requirements.txt)
    return out


def _cert_fingerprint() -> str | None:
    """SHA-256 of the server cert (DER), colon-separated hex, or None."""
    crt = os.path.join(os.path.dirname(__file__), "..", "..", "certs", "server.crt")
    try:
        with open(crt) as f:
            pem = f.read()
        body = pem.split("-----BEGIN CERTIFICATE-----")[1].split("-----END CERTIFICATE-----")[0]
        der = base64.b64decode("".join(body.split()))
        digest = hashlib.sha256(der).hexdigest().upper()
        return ":".join(digest[i:i + 2] for i in range(0, len(digest), 2))
    except (OSError, IndexError, ValueError):
        return None


@router.post("/inject_error")
def inject_error(req: InjectErrorReq) -> dict:
    with store.lock():
        store.state.error_injections.append({
            "path": req.path,
            "status": req.status,
            "count": max(1, req.count),
        })
        return {"queued": True, "injections": list(store.state.error_injections)}


@router.post("/locate")
def inject_locate(req: LocateInjectReq) -> dict:
    """Simulate the InvenTree "locate" button: queue a locate request that the
    HMI will see on its GET /rack poll. Supply one of `slot`, `stock_id`, or
    `part_id` (which lights every slot currently holding that part)."""
    with store.lock():
        targets: list[int] = []
        if req.slot is not None:
            targets = [req.slot]
        elif req.stock_id is not None:
            s = store.find_stock_slot(req.stock_id)
            if s is None:
                raise HTTPException(
                    status_code=404,
                    detail=f"stock item {req.stock_id} is not in a rack slot",
                )
            targets = [s]
        elif req.part_id is not None:
            targets = store.located_slots_for(req.part_id)
            if not targets:
                raise HTTPException(
                    status_code=404,
                    detail=f"part {req.part_id} is not in any rack slot",
                )
        else:
            raise HTTPException(
                status_code=400,
                detail="supply one of 'slot', 'stock_id', or 'part_id'",
            )
        queued = [store.enqueue_locate(n) for n in targets]
        return {"located": bool(queued), "locates": queued}
