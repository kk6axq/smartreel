#!/usr/bin/env bash
# Restart the local InvenTree containers to pick up plugin code changes,
# wait for the API to come back, and confirm the SmartReel plugin mounted.
set -euo pipefail

COMPOSE_DIR=~/Desktop/Projects/inventree
BASE=http://inventree.localhost

cd "$COMPOSE_DIR"
docker compose restart inventree-server inventree-worker >/dev/null

echo -n "waiting for InvenTree"
for i in $(seq 1 45); do
    if curl -sf -m 2 "$BASE/api/" >/dev/null 2>&1; then
        echo " up"
        code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/plugin/smartreel/api/v1/health")
        echo "smartreel health: HTTP $code"
        if [ "$code" != "200" ]; then
            # Plugin URLs aren't mounted. This happens after a container
            # *recreate* (docker compose up -d / .env change) rather than a
            # plain restart: the server boots without collecting plugin URLs,
            # so /plugin/* falls through to the SPA (401 on /health, 302->/web
            # on authed calls). A second clean restart re-collects them.
            echo "plugin URLs not mounted — restarting server once more to remount"
            docker compose restart inventree-server >/dev/null
            for _ in $(seq 1 30); do
                curl -sf -m 2 "$BASE/api/" >/dev/null 2>&1 && break
                sleep 2
            done
            code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/plugin/smartreel/api/v1/health")
            echo "smartreel health after remount: HTTP $code"
        fi
        [ "$code" = "200" ] && exit 0
        exit 1
    fi
    echo -n "."
    sleep 2
done
echo " TIMEOUT"
exit 1
