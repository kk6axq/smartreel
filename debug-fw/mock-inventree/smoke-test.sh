#!/usr/bin/env bash
# Boots the mock on a throwaway HTTP port and exercises the v1 contract
# (docs/hmi-plugin-api.md): rack, resolve, assign, whole-reel pick, job item
# pick, clear, anomaly, locate, provision, op_id idempotency.
#
# Usage: ./smoke-test.sh    (needs .venv from ./setup.sh)
set -euo pipefail
# `check` mutates $fail and is always the last element of a pipeline; without
# lastpipe it runs in a subshell and failures never reach the final verdict.
shopt -s lastpipe
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

PORT=8997
B="http://127.0.0.1:$PORT/api/v1"
H="Authorization: Token dev-token"
PY=./.venv/bin/python

LOG="$(mktemp "${TMPDIR:-/tmp}/mock-smoke.XXXXXX.log")"
SR_HTTPS=0 SR_PORT=$PORT $PY -m uvicorn app.main:app \
    --host 127.0.0.1 --port $PORT >"$LOG" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT

for i in $(seq 1 30); do
    curl -sf "$B/health" >/dev/null 2>&1 && break
    sleep 0.2
done

fail=0
check() {  # check <name> <python-expr over stdin json>
    local name="$1" expr="$2"
    if $PY -c "import json,sys; d=json.load(sys.stdin); assert $expr, d" 2>/dev/null; then
        echo "ok   $name"
    else
        echo "FAIL $name"; fail=1
    fi
}

echo "--- health"
curl -s "$B/health" | check "health ok+version" \
    'd["ok"] and d["version"] == "0.2.0"'

echo "--- rack snapshot"
RACK=$(curl -s -H "$H" "$B/rack")
echo "$RACK" | check "rack shape" \
    'd["location_id"] == 42 and d["n_slots"] == 64 and "pickjobs_available" in d'
echo "$RACK" | check "slots have no chain/state fields (new shape)" \
    '"chain" not in d["slots"][0] and "state" not in d["slots"][0]'

# First occupied + first empty slot, plus the occupied slot's part id.
OCC=$(echo "$RACK"  | $PY -c 'import json,sys; d=json.load(sys.stdin); print(next(s["slot"] for s in d["slots"] if s["stock"]))')
EMPTY=$(echo "$RACK" | $PY -c 'import json,sys; d=json.load(sys.stdin); print(next(s["slot"] for s in d["slots"] if not s["stock"]))')
OCC_SID=$(echo "$RACK" | $PY -c 'import json,sys; d=json.load(sys.stdin); print(next(s["stock"]["id"] for s in d["slots"] if s["stock"]))')
echo "occupied slot=$OCC (stock $OCC_SID), empty slot=$EMPTY"

echo "--- whole-reel pick to staging"
PICK=$(curl -s -H "$H" -H 'Content-Type: application/json' \
    -d '{"op_id":"smoke-pick-1"}' "$B/rack/slots/$OCC/pick")
echo "$PICK" | check "pick moves to 99, no qty math" \
    "d[\"slot\"] == $OCC and d[\"stock_id\"] == $OCC_SID and d[\"moved_to\"] == 99"
curl -s -H "$H" "$B/rack" | check "slot freed after pick" \
    "next(s for s in d[\"slots\"] if s[\"slot\"] == $OCC)[\"stock\"] is None"
# idempotent replay
curl -s -H "$H" -H 'Content-Type: application/json' \
    -d '{"op_id":"smoke-pick-1"}' "$B/rack/slots/$OCC/pick" \
    | check "pick op_id replay returns cached resp" \
        "d[\"stock_id\"] == $OCC_SID"

echo "--- resolve + assign (load flow)"
CODE=$($PY -c 'import json; print(json.load(open("seed.json"))["physical_qr_stock"][0]["barcode"])')
RES=$(curl -s -H "$H" -H 'Content-Type: application/json' \
    -d "{\"code\":\"$CODE\"}" "$B/barcode/resolve")
echo "$RES" | check "resolve stockitem" 'd["type"] == "stockitem" and d["stock"]["slot_num"] is None'
SID=$(echo "$RES" | $PY -c 'import json,sys; print(json.load(sys.stdin)["stock"]["id"])')
curl -s -H "$H" -H 'Content-Type: application/json' \
    -d "{\"stock_item_id\":$SID,\"op_id\":\"smoke-assign-1\"}" \
    "$B/rack/slots/$EMPTY/assign" \
    | check "assign resp shape" "d[\"slot\"] == $EMPTY and d[\"stock\"][\"id\"] == $SID"
curl -s -H "$H" -H 'Content-Type: application/json' \
    -d "{\"stock_item_id\":$SID,\"op_id\":\"smoke-assign-conflict\"}" \
    -o /dev/null -w '%{http_code}\n' "$B/rack/slots/$OCC/assign" | grep -q 200 \
    && echo "ok   re-assign same stock to other slot (implicit vacate)" \
    || { echo "FAIL re-assign"; fail=1; }

echo "--- pick job item (whole reel)"
JOBS=$(curl -s -H "$H" "$B/pickjobs")
echo "$JOBS" | check "jobs derived status + destination" \
    'd["jobs"][0]["status"] in ("pending","partial","done") and "destination_id" in d["jobs"][0]'
# find a job item with a located slot
read -r JID IDX SLOT <<<"$(echo "$JOBS" | $PY -c '
import json, sys
d = json.load(sys.stdin)
for j in d["jobs"]:
    for it in j["items"]:
        if it["located_slots"] and not it["picked"]:
            print(j["id"], it["idx"], it["located_slots"][0]); raise SystemExit
print("none 0 0")')"
if [ "$JID" != "none" ]; then
    curl -s -H "$H" -H 'Content-Type: application/json' \
        -d "{\"slot_num\":$SLOT,\"op_id\":\"smoke-jobpick-1\"}" \
        "$B/pickjobs/$JID/items/$IDX/pick" \
        | check "job item pick ($JID/$IDX@$SLOT)" \
            'd["item"]["picked"] and d["job_status"] in ("partial","done")'
else
    echo "skip job item pick (no locatable item in seed)"
fi

echo "--- clear (anomaly reconcile)"
RACK2=$(curl -s -H "$H" "$B/rack")
OCC2=$(echo "$RACK2" | $PY -c 'import json,sys; d=json.load(sys.stdin); print(next(s["slot"] for s in d["slots"] if s["stock"]))')
curl -s -H "$H" -H 'Content-Type: application/json' \
    -d '{"op_id":"smoke-clear-1","reason":"anomaly:removed"}' \
    "$B/rack/slots/$OCC2/clear" \
    | check "clear moves to pulled (98)" 'd["moved_to"] == 98'
curl -s -H "$H" -H 'Content-Type: application/json' \
    -d '{"op_id":"smoke-clear-2","reason":"anomaly:removed"}' \
    "$B/rack/slots/$OCC2/clear" \
    | check "clear already-empty is 2xx idempotent" 'd["stock_id"] is None'

echo "--- anomaly + locate + provision"
curl -s -H "$H" -H 'Content-Type: application/json' \
    -d '{"kind":"removed","slot_num":5,"detail":"smoke","op_id":"smoke-anom-1"}' \
    "$B/anomaly" | check "anomaly logged" 'd["logged"]'
PARTID=$(echo "$RACK2" | $PY -c 'import json,sys; d=json.load(sys.stdin); print(next(s["stock"]["part"]["id"] for s in d["slots"] if s["stock"]))')
curl -s -H "$H" "$B/parts/locate?part_id=$PARTID" \
    | check "locate shape" 'd["part_id"] and isinstance(d["slots"], list)'

echo "--- locate button (LocateMixin) round-trip"
# simulate InvenTree's locate button for $PARTID, see it on the rack poll, ack it
LID=$(curl -s -H "$H" -H 'Content-Type: application/json' \
    -d "{\"part_id\":\"$PARTID\"}" "$B/_dev/locate" \
    | $PY -c 'import json,sys; d=json.load(sys.stdin); print(d["locates"][0]["id"])')
curl -s -H "$H" "$B/rack" \
    | check "locate shows in rack snapshot" \
        "any(l[\"id\"] == $LID for l in d[\"locates\"])"
curl -s -H "$H" -H 'Content-Type: application/json' \
    -d "{\"ids\":[$LID]}" "$B/rack/locates/ack" \
    | check "ack drops the locate" "all(l[\"id\"] != $LID for l in d[\"locates\"])"

curl -s -H "$H" "$B/_dev/provision" \
    | check "provision payload + svg" \
        'd["payload"].startswith("SRPROV1:") and d["svg"]'

echo "--- register grow"
curl -s -H "$H" -H 'Content-Type: application/json' \
    -d '{"n_slots":80,"op_id":"smoke-reg-1"}' "$B/rack/register" \
    | check "register grows to 80" 'len(d["slot_locations"]) == 80'

echo
if [ "$fail" -eq 0 ]; then echo "ALL OK"; else echo "FAILURES (log: $LOG)"; exit 1; fi
