"""Token auth dependency. Real plugin will share this shape.

Header format: `Authorization: Token <SR_TOKEN>`. We accept `Bearer` too as a
convenience while the HMI is being wired, but the real InvenTree convention is
`Token` so the spec calls that out.
"""
from __future__ import annotations

from fastapi import Header, HTTPException, status

from .config import settings


def require_token(authorization: str | None = Header(default=None)) -> None:
    if not authorization:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="missing Authorization header",
        )
    parts = authorization.split(None, 1)
    if len(parts) != 2 or parts[0].lower() not in ("token", "bearer"):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="expected 'Authorization: Token <token>'",
        )
    if parts[1] != settings.token:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="bad token",
        )
