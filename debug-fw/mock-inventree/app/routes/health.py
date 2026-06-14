"""Liveness probe. No auth so the HMI can ping before it has a token loaded."""
from __future__ import annotations

import datetime as dt

from fastapi import APIRouter

from ..config import settings
from ..models import HealthResp


router = APIRouter(prefix="/api/v1", tags=["health"])


@router.get("/health", response_model=HealthResp)
def health() -> HealthResp:
    return HealthResp(
        ok=True,
        server="smartreel-mock-inventree",
        version=settings.version,
        time=dt.datetime.now(dt.timezone.utc).isoformat(),
    )
