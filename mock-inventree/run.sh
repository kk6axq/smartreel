#!/usr/bin/env bash
# Run the mock InvenTree plugin server. Pass through any extra uvicorn flags.
#
# Defaults to HTTPS on :8443 with the self-signed cert in certs/. Disable TLS
# with SR_HTTPS=0 (then port defaults to 8000).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

if [ ! -d .venv ]; then
    echo "no .venv -- run ./setup.sh first" >&2
    exit 1
fi
HOST="${SR_HOST:-0.0.0.0}"
USE_HTTPS="${SR_HTTPS:-1}"

if [ "$USE_HTTPS" = "1" ]; then
    PORT="${SR_PORT:-8443}"
    if [ ! -f certs/server.crt ] || [ ! -f certs/server.key ]; then
        echo "certs/server.{crt,key} missing -- generating"
        ./gen-cert.sh
    fi
    echo "Serving HTTPS on https://${HOST}:${PORT}  (cert: certs/server.crt)"
    exec ./.venv/bin/uvicorn app.main:app \
        --host "$HOST" --port "$PORT" \
        --ssl-keyfile  certs/server.key \
        --ssl-certfile certs/server.crt \
        "$@"
else
    PORT="${SR_PORT:-8000}"
    echo "Serving plain HTTP on http://${HOST}:${PORT}  (SR_HTTPS=0)"
    exec ./.venv/bin/uvicorn app.main:app --host "$HOST" --port "$PORT" "$@"
fi
