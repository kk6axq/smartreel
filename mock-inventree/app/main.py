"""FastAPI app: app + middleware + lifespan. Route modules live in routes/."""
from __future__ import annotations

import asyncio
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from . import store
from .config import settings
from .seed import reseed
from .routes import anomaly, barcode, dev, health, parts, pickjobs, rack

_STATIC_DIR = os.path.join(os.path.dirname(__file__), "static")


@asynccontextmanager
async def lifespan(app: FastAPI):  # noqa: ARG001
    reseed()
    yield


app = FastAPI(
    title="SmartReel mock InvenTree plugin",
    version=settings.version,
    lifespan=lifespan,
)

# CORS open during dev so a browser at localhost:5173 etc. can poke the API.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.middleware("http")
async def latency_and_inject(request: Request, call_next):
    """Inject configured per-request latency + honor /_dev/inject_error."""
    if settings.latency_ms > 0:
        await asyncio.sleep(settings.latency_ms / 1000.0)
    code = store.consume_error_injection(request.url.path)
    if code is not None:
        return JSONResponse(
            {"detail": f"injected error: status {code} at {request.url.path}"},
            status_code=code,
        )
    return await call_next(request)


# Public, no-auth
app.include_router(health.router)

# Token-gated API
app.include_router(barcode.router)
app.include_router(rack.router)
app.include_router(pickjobs.router)
app.include_router(parts.router)
app.include_router(anomaly.router)

# Dev / test helpers
app.include_router(dev.router)


# ----- status dashboard --------------------------------------------------
# Plain-HTML/JS page that polls the API to render rack state, pick jobs,
# and anomalies. No build step. Token is stored in the browser's
# localStorage; the page is harmless without one (just shows offline).

app.mount("/ui", StaticFiles(directory=_STATIC_DIR, html=True), name="ui")


@app.get("/", include_in_schema=False)
def _root() -> RedirectResponse:
    return RedirectResponse(url="/ui/")
