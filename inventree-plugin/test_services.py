#!/usr/bin/env python3
"""Unit tests for the pure logic in smart_reel/services.py.

The plugin's model-touching code is exercised end-to-end by test_live.py /
test_multirack.py against a running InvenTree. This file covers the pieces
that DON'T need a database — the op_id idempotency cache, wire-id mapping,
and pick-job status — so they can be regression-tested with nothing but a
Python interpreter.

services.py imports only three Django symbols at module load (`cache`,
`transaction`, `timezone`); every model import is lazy and inside a function.
We stub those three before loading the module by file path (sidestepping the
package __init__, which pulls in the full InvenTree plugin base classes).

Run:  pytest test_services.py      (or:  python3 test_services.py)
"""
from __future__ import annotations

import importlib.util
import os
import sys
import types


# --- Stub the three Django symbols services.py needs at import time --------
def _install_django_stubs():
    class _FakeCache:
        """Minimal cache: ignores TTL, stores in a dict (good enough for the
        idempotency logic, which only relies on get/set semantics)."""
        def __init__(self):
            self.store = {}

        def get(self, key, default=None):
            return self.store.get(key, default)

        def set(self, key, value, ttl=None):
            self.store[key] = value

    django = types.ModuleType("django")
    core = types.ModuleType("django.core")
    cache_mod = types.ModuleType("django.core.cache")
    cache_mod.cache = _FakeCache()
    db_mod = types.ModuleType("django.db")
    db_mod.transaction = types.SimpleNamespace(atomic=lambda *a, **k: None)
    utils_mod = types.ModuleType("django.utils")
    utils_mod.timezone = types.SimpleNamespace(now=lambda: None)

    sys.modules.setdefault("django", django)
    sys.modules["django.core"] = core
    sys.modules["django.core.cache"] = cache_mod
    sys.modules["django.db"] = db_mod
    sys.modules["django.utils"] = utils_mod
    return cache_mod.cache


_FAKE_CACHE = _install_django_stubs()


def _load_services():
    path = os.path.join(os.path.dirname(__file__), "smart_reel", "services.py")
    spec = importlib.util.spec_from_file_location("smart_reel_services", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


services = _load_services()


# --- op_id idempotency cache ----------------------------------------------

def test_op_lookup_miss_returns_none():
    assert services.op_lookup("never-seen") is None


def test_op_remember_then_lookup_roundtrips():
    resp = {"ok": True, "slot": 7}
    services.op_remember("op-123", resp)
    assert services.op_lookup("op-123") == resp


def test_op_none_id_is_noop():
    # A missing op_id must never be cached and must always look up as a miss.
    services.op_remember(None, {"ok": True})
    assert services.op_lookup(None) is None
    assert None not in _FAKE_CACHE.store
    assert not any(k.endswith("None") for k in _FAKE_CACHE.store)


def test_op_id_is_namespaced():
    services.op_remember("abc", {"v": 1})
    assert "smartreel:op:abc" in _FAKE_CACHE.store


# --- part wire id ----------------------------------------------------------

class _FakePart:
    def __init__(self, pk, ipn=None):
        self.pk = pk
        self.IPN = ipn


def test_part_wire_id_prefers_ipn():
    assert services.part_wire_id(_FakePart(42, "RES-0402-10K")) == "RES-0402-10K"


def test_part_wire_id_falls_back_to_pk():
    assert services.part_wire_id(_FakePart(42, None)) == "42"
    assert services.part_wire_id(_FakePart(42, "")) == "42"


# --- pick-job status -------------------------------------------------------

def _job(*picked):
    return {"items": [{"picked": p} for p in picked]}


def test_job_status_pending_when_none_picked():
    assert services.job_status(_job(False, False)) == "pending"


def test_job_status_partial_when_some_picked():
    assert services.job_status(_job(True, False)) == "partial"


def test_job_status_done_when_all_picked():
    assert services.job_status(_job(True, True)) == "done"


def test_job_status_empty_job_is_pending():
    # No items => nothing picked => not "done".
    assert services.job_status(_job()) == "pending"


# --- self-running fallback (no pytest required) ----------------------------

if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items())
           if k.startswith("test_") and callable(v)]
    failures = 0
    for fn in fns:
        try:
            fn()
            print(f"ok   {fn.__name__}")
        except AssertionError as e:
            failures += 1
            print(f"FAIL {fn.__name__}: {e}")
    print(f"\n{len(fns)} tests, {failures} failures")
    sys.exit(1 if failures else 0)
