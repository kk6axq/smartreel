#!/usr/bin/env bash
# Generate a self-signed TLS cert for the mock server.
#
# Cert is at certs/server.{crt,key}, valid 825 days (the macOS-imposed max for
# a SAN cert, mirrored here so the same cert works everywhere). SANs cover the
# obvious dev targets: localhost, 127.0.0.1, the machine's hostname, and the
# LAN IP if we can guess one. The HMI client can pin the SHA-256 fingerprint
# we print at the end, or trust the CN out-of-band.
#
# Re-running is a no-op if the cert exists and has >30 days left. Force a
# rebuild with: rm -rf certs/
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

mkdir -p certs
CRT=certs/server.crt
KEY=certs/server.key

# Skip if a healthy cert is already in place.
if [ -f "$CRT" ] && [ -f "$KEY" ]; then
    if openssl x509 -in "$CRT" -noout -checkend $((30 * 86400)) >/dev/null 2>&1; then
        echo "certs/: existing cert is still valid for 30+ days, skipping"
        openssl x509 -in "$CRT" -noout -fingerprint -sha256 | sed 's/^/  /'
        exit 0
    fi
    echo "certs/: existing cert is near expiry, regenerating"
fi

HOSTNAME_VAL="$(hostname)"
# Best-effort LAN IPv4 (first non-loopback). Quiet if `ip` isn't available.
LAN_IP="$(ip -4 -o addr show 2>/dev/null | awk '$2 != "lo" {print $4}' | cut -d/ -f1 | head -n1 || true)"

SAN_ENTRIES=(
    "DNS:localhost"
    "DNS:smartreel-mock"
    "DNS:smartreel-mock.local"
    "DNS:${HOSTNAME_VAL}"
    "IP:127.0.0.1"
    "IP:::1"
)
[ -n "${LAN_IP:-}" ] && SAN_ENTRIES+=("IP:${LAN_IP}")
SAN_LIST="$(IFS=,; echo "${SAN_ENTRIES[*]}")"

echo "Generating self-signed cert"
echo "  SAN: ${SAN_LIST}"

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$KEY" -out "$CRT" \
    -days 825 \
    -subj "/CN=smartreel-mock" \
    -addext "subjectAltName=${SAN_LIST}" \
    -addext "keyUsage=digitalSignature,keyEncipherment" \
    -addext "extendedKeyUsage=serverAuth" \
    2>/dev/null
chmod 600 "$KEY"

echo "  wrote $CRT"
echo "  wrote $KEY  (mode 600)"
echo "  SHA-256 fingerprint (pin this in the HMI):"
openssl x509 -in "$CRT" -noout -fingerprint -sha256 | sed 's/^/    /'
