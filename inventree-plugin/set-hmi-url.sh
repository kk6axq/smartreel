#!/usr/bin/env bash
# Restart InvenTree (pick up plugin changes), set the SmartReel plugin's
# HMI_URL setting, and (optionally) verify the provisioning payload embeds it.
#
# Usage: ./set-hmi-url.sh [HMI_URL] [RACK_LOCATION_PK]
#   HMI_URL           HTTPS URL the rack should reach the server at (default below)
#   RACK_LOCATION_PK  StockLocation pk of the rack; if given, the provisioning
#                     payload is fetched and printed as a sanity check.
#
# Dev-instance credentials default to admin:inventree; override with env vars:
#   INVENTREE_USER=... INVENTREE_PASS=... ./set-hmi-url.sh ...
set -euo pipefail
cd "$(dirname "$0")"

HMI_URL="${1:-https://inventree.local}"
RACK_PK="${2:-}"
BASE="${INVENTREE_BASE:-http://inventree.localhost}"
USER="${INVENTREE_USER:-admin}"
PASS="${INVENTREE_PASS:-inventree}"

./dev-restart.sh

TOK=$(curl -s -u "$USER:$PASS" "$BASE/api/user/token/?name=smartreel-test" \
      | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')

curl -s -X PATCH -H "Authorization: Token $TOK" -H 'Content-Type: application/json' \
     -d "{\"value\": \"$HMI_URL\"}" \
     "$BASE/api/plugins/smartreel/settings/HMI_URL/" >/dev/null

echo "HMI_URL set to $HMI_URL"

# /provision requires a ?location=<pk> (the rack's StockLocation). Skip the
# verification fetch unless a rack pk was supplied.
if [ -n "$RACK_PK" ]; then
    echo -n "provision payload: "
    curl -s -H "Authorization: Token $TOK" \
         "$BASE/plugin/smartreel/api/v1/provision?location=$RACK_PK" \
         | python3 -c 'import json,sys; print(json.load(sys.stdin)["payload"][:120])'
else
    echo "(pass a rack location pk as the 2nd arg to verify the provision payload)"
fi
