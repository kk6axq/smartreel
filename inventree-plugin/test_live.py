#!/usr/bin/env python3
"""Live contract test for the SmartReel plugin against the local InvenTree.

Run via ./test-live.sh. Idempotent: safe to re-run; it reuses seeded data.

Flow: admin token → enable plugin URL integration + activate plugin →
seed locations/parts/stock/build → exercise every endpoint in
docs/hmi-plugin-api.md → PASS/FAIL summary (exit 1 on any failure).
"""
from __future__ import annotations

import base64
import json
import sys
import time
import urllib.error
import urllib.request

BASE = "http://inventree.localhost"
ADMIN = ("admin", "inventree")

failures: list[str] = []


def check(name: str, cond: bool, extra=""):
    print(f"{'ok  ' if cond else 'FAIL'} {name}" + (f"  {extra}" if extra and not cond else ""))
    if not cond:
        failures.append(name)


def req(method: str, path: str, body=None, token=None, basic=None, raw=False):
    """Returns (status, parsed-json-or-text)."""
    url = BASE + path
    data = None if body is None else json.dumps(body).encode()
    r = urllib.request.Request(url, data=data, method=method)
    r.add_header("Accept", "application/json")
    if body is not None:
        r.add_header("Content-Type", "application/json")
    if token:
        r.add_header("Authorization", f"Token {token}")
    elif basic:
        cred = base64.b64encode(f"{basic[0]}:{basic[1]}".encode()).decode()
        r.add_header("Authorization", f"Basic {cred}")
    try:
        with urllib.request.urlopen(r, timeout=20) as resp:
            text = resp.read().decode()
            return resp.status, (text if raw else (json.loads(text) if text else {}))
    except urllib.error.HTTPError as e:
        text = e.read().decode()
        try:
            return e.code, json.loads(text)
        except json.JSONDecodeError:
            return e.code, {"detail": text[:300]}


def wait_api(timeout=90):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            st, _ = req("GET", "/api/", basic=ADMIN)
            if st == 200:
                return
        except OSError:
            pass
        time.sleep(2)
    sys.exit("InvenTree API did not come up")


def main():
    wait_api()

    # ---- admin token ----
    st, d = req("GET", "/api/user/token/?name=smartreel-test", basic=ADMIN)
    assert st == 200, d
    tok = d["token"]

    # ---- global URL integration + plugin activation ----
    st, d = req("GET", "/api/settings/global/ENABLE_PLUGINS_URL/", token=tok)
    if str(d.get("value")).lower() != "true":
        req("PATCH", "/api/settings/global/ENABLE_PLUGINS_URL/", {"value": "True"}, token=tok)
        print("note: enabled ENABLE_PLUGINS_URL (restart containers + rerun if health 404s)")
    st, d = req("PATCH", "/api/plugins/smartreel/activate/", {"active": True}, token=tok)
    check("plugin activate", st in (200, 201), str(d))

    P = "/plugin/smartreel/api/v1"
    st, d = req("GET", f"{P}/health")
    check("health (no auth)", st == 200 and d.get("ok") is True, str(d))
    if st != 200:
        print("plugin URLs not mounted — restart inventree-server/worker and rerun")
        sys.exit(1)

    # ---- seed: locations ----
    def ensure_location(name, structural=False, parent=None):
        from urllib.parse import quote

        st, d = req("GET", f"/api/stock/location/?name={quote(name)}", token=tok)
        items = d if isinstance(d, list) else d.get("results", [])
        for it in items:
            if it["name"] == name and it.get("parent") == parent:
                return it["pk"]
        body = {"name": name, "structural": structural}
        if parent:
            body["parent"] = parent
        st, d = req("POST", "/api/stock/location/", body, token=tok)
        assert st == 201, d
        return d["pk"]

    rack_pk = ensure_location("SmartReel Rack", structural=True)
    staging_pk = ensure_location("Staging")
    pulled_pk = ensure_location("Pulled")
    inbox_pk = ensure_location("Receiving Inbox")

    for key, val in (("STAGING_LOCATION", staging_pk),
                     ("PULLED_LOCATION", pulled_pk)):
        st, d = req("PATCH", f"/api/plugins/smartreel/settings/{key}/",
                    {"value": str(val)}, token=tok)
        check(f"setting {key}", st == 200, str(d))

    # Provision the rack -> a token bound to it (rtok). Every rack-scoped
    # plugin call below uses rtok, exactly as the HMI does. (No global
    # rack fallback: an unbound token is rejected.)
    st, d = req("GET", f"{P}/provision?location={rack_pk}&rotate=1", token=tok)
    check("provision rack", st == 200
          and d.get("rack", {}).get("location_id") == rack_pk
          and str(d.get("payload", "")).startswith("SRPROV1:"), str(d)[:200])
    check("provision QR svg", bool(d.get("svg")), "no qrcode lib in container?")
    rtok = json.loads(d["payload"].split("SRPROV1:")[1])["t"]
    # An unbound token (the plain admin token) must be refused.
    st, d = req("GET", f"{P}/rack", token=tok)
    check("unbound token refused (409)", st == 409, str(st))

    # ---- seed: parts ----
    def ensure_part(name, ipn, **extra):
        st, d = req("GET", f"/api/part/?IPN={ipn}", token=tok)
        items = d if isinstance(d, list) else d.get("results", [])
        if items:
            return items[0]["pk"]
        st, d = req("POST", "/api/part/",
                    {"name": name, "IPN": ipn, "description": name, **extra}, token=tok)
        assert st == 201, d
        return d["pk"]

    r10k = ensure_part("RES 10k 1% 0805", "R-10K-0805", component=True)
    c100n = ensure_part("CAP 100n X7R 0805", "C-100N-0805", component=True)
    asm = ensure_part("Test PCB Assembly", "ASM-TEST-1", assembly=True, component=False)

    # ---- seed: BOM + build ----
    st, d = req("GET", f"/api/bom/?part={asm}", token=tok)
    bom_items = d if isinstance(d, list) else d.get("results", [])
    have = {it["sub_part"] for it in bom_items}
    for sub, qty in ((r10k, 10), (c100n, 20)):
        if sub not in have:
            st, d = req("POST", "/api/bom/",
                        {"part": asm, "sub_part": sub, "quantity": qty}, token=tok)
            assert st == 201, d

    st, d = req("GET", f"/api/build/?part={asm}", token=tok)
    builds = d if isinstance(d, list) else d.get("results", [])
    if builds:
        build_pk, build_ref = builds[0]["pk"], builds[0]["reference"]
    else:
        st, d = req("POST", "/api/build/",
                    {"part": asm, "quantity": 5, "title": "SmartReel test build"},
                    token=tok)
        assert st == 201, d
        build_pk, build_ref = d["pk"], d["reference"]

    # ---- seed: stock (fresh items each run; cheap and avoids state coupling) ----
    def new_stock(part, qty):
        st, d = req("POST", "/api/stock/",
                    {"part": part, "quantity": qty, "location": inbox_pk}, token=tok)
        assert st == 201, d
        if isinstance(d, list):
            return d[0]["pk"]
        return d["pk"] if "pk" in d else d["items"][0]["pk"]

    si_a = new_stock(r10k, 5000)
    si_b = new_stock(c100n, 4000)

    # =====================================================================
    # Contract exercises (all via the plugin API, token auth)
    # =====================================================================
    run = str(int(time.time()))

    st, d = req("POST", f"{P}/rack/register", {"n_slots": 8, "op_id": f"t-{run}-reg"}, token=rtok)
    check("register 8 slots", st == 200 and len(d.get("slot_locations", [])) >= 8, str(d))

    st, d = req("GET", f"{P}/rack", token=rtok)
    check("rack snapshot", st == 200 and d.get("location_id") == rack_pk
          and d.get("n_slots", 0) >= 8, str(d))
    empty_slots = [s["slot"] for s in d.get("slots", []) if not s["stock"]]
    check("rack has ≥2 empty slots", len(empty_slots) >= 2, str(d)[:200])
    s1, s2 = empty_slots[0], empty_slots[1]

    # resolve: InvenTree-generated json barcode for stock item A
    code = json.dumps({"stockitem": si_a})
    st, d = req("POST", f"{P}/barcode/resolve", {"code": code}, token=rtok)
    check("resolve stockitem QR", st == 200 and d.get("type") == "stockitem"
          and d["stock"]["id"] == si_a and d["stock"]["slot_num"] is None, str(d))

    # resolve unknown
    st, d = req("POST", f"{P}/barcode/resolve", {"code": "garbage-xyz-123"}, token=rtok)
    check("resolve unknown", st == 200 and d.get("type") == "unknown", str(d))

    # assign A to slot s1
    st, d = req("POST", f"{P}/rack/slots/{s1}/assign",
                {"stock_item_id": si_a, "op_id": f"t-{run}-as1"}, token=rtok)
    check("assign A→slot", st == 200 and d.get("slot") == s1
          and d["stock"]["id"] == si_a, str(d))

    # conflict: B into same slot
    st, d = req("POST", f"{P}/rack/slots/{s1}/assign",
                {"stock_item_id": si_b, "op_id": f"t-{run}-as2"}, token=rtok)
    check("assign conflict 409", st == 409, str(d))

    # resolve A again: now reports its slot
    st, d = req("POST", f"{P}/barcode/resolve", {"code": code}, token=rtok)
    check("resolve shows slot_num", st == 200 and d["stock"]["slot_num"] == s1, str(d))

    # whole-reel pick from s1 → staging
    st, d = req("POST", f"{P}/rack/slots/{s1}/pick", {"op_id": f"t-{run}-p1"}, token=rtok)
    check("pick → staging", st == 200 and d.get("moved_to") == staging_pk
          and d.get("stock_id") == si_a, str(d))
    st, d = req("GET", f"/api/stock/{si_a}/", token=tok)
    check("stock A really moved", st == 200 and d.get("location") == staging_pk, str(d))
    # idempotent replay
    st, d = req("POST", f"{P}/rack/slots/{s1}/pick", {"op_id": f"t-{run}-p1"}, token=rtok)
    check("pick op_id replay", st == 200 and d.get("stock_id") == si_a, str(d))
    # picking the now-empty slot with a new op_id → 409
    st, d = req("POST", f"{P}/rack/slots/{s1}/pick", {"op_id": f"t-{run}-p2"}, token=rtok)
    check("pick empty slot 409", st == 409, str(d))

    # clear: B into s2, then clear → pulled
    st, d = req("POST", f"{P}/rack/slots/{s2}/assign",
                {"stock_item_id": si_b, "op_id": f"t-{run}-as3"}, token=rtok)
    check("assign B→slot2", st == 200, str(d))
    st, d = req("POST", f"{P}/rack/slots/{s2}/clear",
                {"op_id": f"t-{run}-c1", "reason": "anomaly:removed"}, token=rtok)
    check("clear → pulled", st == 200 and d.get("moved_to") == pulled_pk, str(d))
    st, d = req("POST", f"{P}/rack/slots/{s2}/clear",
                {"op_id": f"t-{run}-c2", "reason": "anomaly:removed"}, token=rtok)
    check("clear empty idempotent", st == 200 and d.get("stock_id") is None, str(d))

    # pick job from build (web-panel endpoint: explicit target rack)
    st, d = req("POST", f"{P}/pickjobs/from-build",
                {"build_id": build_pk, "rack_location_id": rack_pk,
                 "op_id": f"t-{run}-fb"}, token=tok)
    check("from-build", st == 200 and d.get("id") == build_ref
          and d.get("rack_id") == rack_pk
          and len(d.get("items", [])) == 2 and d.get("status") == "pending", str(d))

    # stock a reel of r10k into a slot so item 0 becomes locatable
    si_c = new_stock(r10k, 3000)
    st, d = req("POST", f"{P}/rack/slots/{s1}/assign",
                {"stock_item_id": si_c, "op_id": f"t-{run}-as4"}, token=rtok)
    check("assign C→slot1", st == 200, str(d))

    st, d = req("GET", f"{P}/pickjobs", token=rtok)
    jobs = d.get("jobs", [])
    job = next((j for j in jobs if j["id"] == build_ref), None)
    check("pickjobs lists job", job is not None, str(d)[:300])
    it0 = job["items"][0] if job else {}
    check("item0 located in slot1", s1 in it0.get("located_slots", []), str(it0))

    # wrong-slot pick → 409 (slot1 holds r10k; item 1 wants c100n)
    st, d = req("POST", f"{P}/pickjobs/{build_ref}/items/1/pick",
                {"slot_num": s1, "op_id": f"t-{run}-jp0"}, token=rtok)
    check("job pick wrong part 409", st == 409, str(d))

    # correct pick
    st, d = req("POST", f"{P}/pickjobs/{build_ref}/items/0/pick",
                {"slot_num": s1, "op_id": f"t-{run}-jp1"}, token=rtok)
    check("job item pick", st == 200 and d["item"]["picked"] is True
          and d["job_status"] == "partial", str(d))
    st, d = req("GET", f"/api/stock/{si_c}/", token=tok)
    check("job-picked reel in staging", d.get("location") == staging_pk, str(d))

    # locate
    st, d = req("GET", f"{P}/parts/locate?part_id=R-10K-0805", token=rtok)
    check("locate", st == 200 and d.get("part_id") == "R-10K-0805", str(d))

    # anomaly
    st, d = req("POST", f"{P}/anomaly",
                {"kind": "removed", "slot_num": s2, "detail": "test yank",
                 "op_id": f"t-{run}-an1"}, token=rtok)
    check("anomaly logged", st == 200 and d.get("logged") is True, str(d))

    # auth: no token → 401/403
    st, d = req("GET", f"{P}/rack")
    check("rack requires auth", st in (401, 403), str(st))

    print()
    if failures:
        print(f"{len(failures)} FAILURE(S): {failures}")
        sys.exit(1)
    print("ALL OK")


if __name__ == "__main__":
    main()
