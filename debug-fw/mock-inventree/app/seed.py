"""Deterministic seed builder.

Reads `seed.json` and populates `store.state`:
  - parts catalog
  - eight physical-QR stock items (matching scripts/gen_mock_qr.py output)
    parked in the rack location but not in any slot
  - ~60% of the 64 slots pre-populated with synthetic stock items, using the
    same LCG and constants as esp32-hmi/src/ui/app_state.cpp:build_rack() so
    the rack layout feels familiar when the HMI switches to API mode
  - the three pick jobs from the HMI mock (BO-0042/3/4)

`reseed()` is idempotent: it resets state before populating, so /_dev/reset
can call it repeatedly.
"""
from __future__ import annotations

import json
import os

from .config import settings
from . import store


SEED_PATH = os.path.join(os.path.dirname(__file__), "..", "seed.json")


class _LCG:
    """Mirrors the HMI's PRNG so seeded slots line up across the boundary."""
    def __init__(self, seed: int) -> None:
        self.s = seed & 0xFFFFFFFF

    def next(self) -> int:
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s


def _empty_slot(slot_num: int) -> dict:
    chain = (slot_num - 1) // settings.slots_per_chain + 1
    pos   = (slot_num - 1) %  settings.slots_per_chain + 1
    return {
        "slot": slot_num,
        "chain": chain,
        "position": pos,
        "state": "EMPTY",
        "location_id": store.slot_location_id(slot_num),
        "stock_id": None,
    }


def _alloc_stock_id() -> int:
    sid = store.state.next_stock_id
    store.state.next_stock_id += 1
    return sid


def reseed() -> None:
    """Reset state and repopulate from seed.json."""
    with open(SEED_PATH) as f:
        data = json.load(f)

    with store.lock():
        s = store.state
        s.parts.clear()
        s.stock_items.clear()
        s.slots.clear()
        s.pick_jobs.clear()
        s.anomalies.clear()
        s.locates.clear()
        s.op_cache.clear()
        s.error_injections.clear()
        s.next_stock_id = 1000
        s.next_anomaly_id = 1
        s.next_locate_id = 1

        # Parts catalogue
        for p in data["parts"]:
            s.parts[p["id"]] = dict(p)

        # Empty rack scaffold
        s.slots = [_empty_slot(i) for i in range(1, settings.n_slots + 1)]

        # Eight physical-QR stock items, parked in the rack location with
        # no slot assignment. Scanning one drives the HMI's load flow.
        for entry in data["physical_qr_stock"]:
            if entry["part_id"] not in s.parts:
                # seed.json typo -- skip rather than crash at boot
                continue
            sid = _alloc_stock_id()
            s.stock_items[sid] = {
                "id": sid,
                "part_id": entry["part_id"],
                "qty": entry["qty"],
                "batch": entry["batch"],
                "barcode": entry["barcode"],
                "location_id": settings.rack_location_id,
            }

        # Pre-populate ~60% of slots from the catalogue using the HMI's PRNG.
        # Each occupied slot gets a fresh synthetic stock item with an
        # `SR-SI-####` barcode (no printed QR exists for these; they only
        # appear via /rack on boot).
        catalog_ids = list(s.parts.keys())
        rng = _LCG(0xCAFE1234)
        for slot in s.slots:
            occupied = (rng.next() % 100) < 60
            if not occupied:
                continue
            part_id = catalog_ids[rng.next() % len(catalog_ids)]
            qty = 50 + (rng.next() % 1450)
            sid = _alloc_stock_id()
            s.stock_items[sid] = {
                "id": sid,
                "part_id": part_id,
                "qty": qty,
                "batch": f"B-S{slot['slot']:02d}",
                "barcode": f"SR-SI-{sid:04d}",
                "location_id": slot["location_id"],
            }
            slot["state"] = "OCCUPIED"
            slot["stock_id"] = sid

        # Pick jobs (status is derived from item state; see store.job_status)
        for j in data["pick_jobs"]:
            s.pick_jobs[j["id"]] = {
                "id": j["id"],
                "name": j["name"],
                "requested_at": j["requested_at"],
                "destination_id": j.get("destination_id", settings.staging_location_id),
                "items": [
                    {
                        "idx": i,
                        "part_id": it["part_id"],
                        "qty": it["qty"],
                        "picked": False,
                    }
                    for i, it in enumerate(j["items"])
                ],
            }
