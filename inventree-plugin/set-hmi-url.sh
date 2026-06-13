#!/usr/bin/env bash
# Restart InvenTree (pick up plugin changes), set the SmartReel plugin's
# HMI_URL setting, and verify the provisioning payload embeds it.
#
# Usage: ./set-hmi-url.sh [https://192.168.1.235]
set -euo pipefail
cd "$(dirname "$0")"

HMI_URL="${1:-https://192.168.1.235}"
BASE=http://inventree.localhost

./dev-restart.sh

TOK=$(curl -s -u admin:inventree "$BASE/api/user/token/?name=smartreel-test" \
      | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')

curl -s -X PATCH -H "Authorization: Token $TOK" -H 'Content-Type: application/json' \
     -d "{\"value\": \"$HMI_URL\"}" \
     "$BASE/api/plugins/smartreel/settings/HMI_URL/" >/dev/null

echo "HMI_URL set to $HMI_URL"
echo -n "provision payload: "
curl -s -H "Authorization: Token $TOK" "$BASE/plugin/smartreel/api/v1/provision" \
     | python3 -c 'import json,sys; print(json.load(sys.stdin)["payload"][:120])'
