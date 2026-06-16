#!/usr/bin/env python3
"""Multi-rack live test for the SmartReel plugin.

Provisions two independent racks (A and B), each with its own token, and
verifies the token scopes every rack-relative operation: registration,
snapshot, assign/pick, and pick-job targeting are isolated per rack.

Run via ./test-multirack.sh (after the stack is up + plugin loaded).
"""
from __future__ import annotations

import base64
import json
import sys
import time
import urllib.error
import urllib.request
from urllib.parse import quote

BASE = "http://inventree.localhost"
ADMIN = ("admin", "inventree")
failures: list[str] = []


def check(name, cond, extra=""):
    print(f"{'ok  ' if cond else 'FAIL'} {name}" + (f"  {extra}" if extra and not cond else ""))
    if not cond:
        failures.append(name)


def req(method, path, body=None, token=None, basic=None):
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
            t = resp.read().decode()
            return resp.status, (json.loads(t) if t else {})
    except urllib.error.HTTPError as e:
        t = e.read().decode()
        try:
            return e.code, json.loads(t)
        except json.JSONDecodeError:
            return e.code, {"detail": t[:200]}


def main():
    st, _ = req("GET", "/api/", basic=ADMIN)
    if st != 200:
        sys.exit("InvenTree API not up")
    tok = req("GET", "/api/user/token/?name=smartreel-test", basic=ADMIN)[1]["token"]

    P = "/plugin/smartreel/api/v1"
    run = str(int(time.time()))   # unique op_ids per run (idempotency cache is 10 min)

    # --- two rack locations ---
    def ensure_location(name, structural=False):
        st, d = req("GET", f"/api/stock/location/?name={quote(name)}", token=tok)
        items = d if isinstance(d, list) else d.get("results", [])
        for it in items:
            if it["name"] == name:
                return it["pk"]
        st, d = req("POST", "/api/stock/location/",
                    {"name": name, "structural": structural}, token=tok)
        assert st == 201, d
        return d["pk"]

    rack_a = ensure_location("SmartReel Rack A", structural=True)
    rack_b = ensure_location("SmartReel Rack B", structural=True)

    # --- provision each rack (binds a token to it) ---
    st, da = req("GET", f"{P}/provision?location={rack_a}&rotate=1", token=tok)
    check("provision rack A", st == 200 and da.get("rack", {}).get("location_id") == rack_a, str(da))
    st, db = req("GET", f"{P}/provision?location={rack_b}&rotate=1", token=tok)
    check("provision rack B", st == 200 and db.get("rack", {}).get("location_id") == rack_b, str(db))
    tok_a = json.loads(da["payload"].split("SRPROV1:")[1])["t"]
    tok_b = json.loads(db["payload"].split("SRPROV1:")[1])["t"]
    check("distinct per-rack tokens", tok_a != tok_b and tok_a and tok_b)

    # --- /racks lists both ---
    st, d = req("GET", f"{P}/racks", token=tok)
    rack_ids = {r["location_id"] for r in d.get("racks", [])}
    check("racks list includes A and B", rack_a in rack_ids and rack_b in rack_ids, str(rack_ids))

    # --- register slots via each token (token carries the rack) ---
    st, d = req("POST", f"{P}/rack/register", {"n_slots": 6, "op_id": f"mr-{run}-regA"}, token=tok_a)
    check("register A via token A -> rack A", st == 200 and d.get("location_id") == rack_a, str(d))
    st, d = req("POST", f"{P}/rack/register", {"n_slots": 4, "op_id": f"mr-{run}-regB"}, token=tok_b)
    check("register B via token B -> rack B", st == 200 and d.get("location_id") == rack_b, str(d))

    # --- snapshots are rack-scoped and distinct ---
    st, sa = req("GET", f"{P}/rack", token=tok_a)
    st, sb = req("GET", f"{P}/rack", token=tok_b)
    check("snapshot A = rack A, 6 slots", sa.get("location_id") == rack_a and sa.get("n_slots") == 6, str(sa)[:160])
    check("snapshot B = rack B, 4 slots", sb.get("location_id") == rack_b and sb.get("n_slots") == 4, str(sb)[:160])

    # --- seed two stock items, assign one into each rack ---
    inbox = ensure_location("Receiving Inbox")
    # reuse the R-10K part from the single-rack test if present, else make one
    st, d = req("GET", "/api/part/?IPN=R-10K-0805", token=tok)
    parts = d if isinstance(d, list) else d.get("results", [])
    if parts:
        part = parts[0]["pk"]
    else:
        part = req("POST", "/api/part/",
                   {"name": "RES 10k", "IPN": "R-10K-0805", "description": "r", "component": True},
                   token=tok)[1]["pk"]

    def new_stock(qty):
        st, d = req("POST", "/api/stock/", {"part": part, "quantity": qty, "location": inbox}, token=tok)
        assert st == 201, d
        return (d[0]["pk"] if isinstance(d, list) else d.get("pk") or d["items"][0]["pk"])

    si_a, si_b = new_stock(1000), new_stock(2000)

    # Use a currently-empty slot in each rack (state accumulates across runs).
    def empty_slot(snap):
        return next((s["slot"] for s in snap["slots"] if not s["stock"]), None)
    ea, eb = empty_slot(sa), empty_slot(sb)
    check("rack A has an empty slot", ea is not None)
    check("rack B has an empty slot", eb is not None)

    ra = req("POST", f"{P}/rack/slots/{ea}/assign",
             {"stock_item_id": si_a, "op_id": f"mr-{run}-asA"}, token=tok_a)
    rb = req("POST", f"{P}/rack/slots/{eb}/assign",
             {"stock_item_id": si_b, "op_id": f"mr-{run}-asB"}, token=tok_b)
    check("assign A ok", ra[0] == 200, str(ra))
    check("assign B ok", rb[0] == 200, str(rb))

    # --- isolation: each reel landed only in its own rack ---
    sa = req("GET", f"{P}/rack", token=tok_a)[1]
    sb = req("GET", f"{P}/rack", token=tok_b)[1]
    a_ids = {(s.get("stock") or {}).get("id") for s in sa["slots"]}
    b_ids = {(s.get("stock") or {}).get("id") for s in sb["slots"]}
    check("rack A holds si_a, not si_b", si_a in a_ids and si_b not in a_ids, str(a_ids))
    check("rack B holds si_b, not si_a", si_b in b_ids and si_a not in b_ids, str(b_ids))

    # --- a build-order job targeted at rack B is invisible to rack A ---
    st, d = req("GET", "/api/build/", token=tok)
    builds = d if isinstance(d, list) else d.get("results", [])
    if builds:
        build_pk = builds[0]["pk"]
        ref = builds[0]["reference"]
        # Select si_b (housed in rack B) -> the job fans out to rack B only
        # (review item 10b: jobs follow the racks holding the selected reels).
        st, d = req("POST", f"{P}/pickjobs/from-build",
                    {"build_id": build_pk, "stock_ids": [si_b],
                     "op_id": f"mr-{run}-jobB"}, token=tok)
        jobs = d.get("jobs", [])
        check("create job targeting rack B", st == 200 and len(jobs) == 1
              and jobs[0].get("rack_id") == rack_b, str(d)[:200])
        a_jobs = {j["id"] for j in req("GET", f"{P}/pickjobs", token=tok_a)[1].get("jobs", [])}
        b_jobs = {j["id"] for j in req("GET", f"{P}/pickjobs", token=tok_b)[1].get("jobs", [])}
        check("job visible to rack B only", ref in b_jobs and ref not in a_jobs,
              f"A={a_jobs} B={b_jobs}")
    else:
        print("skip job-targeting (no build order seeded)")

    # --- an unbound token is rejected (no global fallback) ---
    st, d = req("GET", f"{P}/rack", token=tok)
    check("unbound admin token rejected (409)", st == 409, str(st))

    print()
    if failures:
        print(f"{len(failures)} FAILURE(S): {failures}")
        sys.exit(1)
    print("ALL OK")


if __name__ == "__main__":
    main()
