"""Runtime config -- all from environment variables, all optional."""
from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    token: str = os.environ.get("SR_TOKEN", "dev-token")
    host: str = os.environ.get("SR_HOST", "0.0.0.0")
    port: int = int(os.environ.get("SR_PORT", "8000"))
    rack_location_id: int = int(os.environ.get("SR_RACK_LOCATION_ID", "42"))
    # Default destination for picked reels (user stories: "Staging").
    staging_location_id: int = int(os.environ.get("SR_STAGING_LOCATION_ID", "99"))
    # Destination for anomaly /clear removals ("Unsorted / pulled").
    pulled_location_id: int = int(os.environ.get("SR_PULLED_LOCATION_ID", "98"))
    n_chains: int = int(os.environ.get("SR_N_CHAINS", "4"))
    slots_per_chain: int = int(os.environ.get("SR_SLOTS_PER_CHAIN", "16"))
    # Artificial per-request latency (ms) to exercise HMI loading states.
    latency_ms: int = int(os.environ.get("SR_LATENCY_MS", "0"))
    # How long an op_id->response entry stays cached for idempotency.
    op_cache_ttl_s: int = int(os.environ.get("SR_OP_CACHE_TTL_S", "600"))

    @property
    def n_slots(self) -> int:
        return self.n_chains * self.slots_per_chain

    @property
    def version(self) -> str:
        return "0.2.0"


settings = Settings()
