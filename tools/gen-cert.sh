#!/bin/bash
# Generate self-signed certificate for bless HTTPS/WSS support
# Usage: ./tools/gen-cert.sh [output-dir]
# Default output: conf/ssl/

set -euo pipefail

DIR="${1:-conf/ssl}"
mkdir -p "$DIR"

KEY="$DIR/key.pem"
CERT="$DIR/cert.pem"
PEM="$DIR/ssl.pem"

# Skip if already exists
if [ -f "$PEM" ]; then
    echo "Certificate already exists: $PEM"
    openssl x509 -in "$PEM" -noout -subject -dates 2>/dev/null || true
    exit 0
fi

DAYS=3650
SUBJ="/CN=bless/O=Bless/O=Injector/C=CN"

echo "Generating self-signed certificate (${DAYS} days)..."
openssl req -x509 -nodes -days "$DAYS" -newkey rsa:2048 \
    -keyout "$KEY" -out "$CERT" -subj "$SUBJ" 2>/dev/null

# civetweb expects key+cert concatenated
cat "$CERT" "$KEY" > "$PEM"
chmod 600 "$KEY" "$PEM"

echo "Done:"
echo "  $PEM"
echo ""
echo "Add to config.yaml:"
echo "  server:"
echo "    options:"
echo '      listening_ports: "8000,8443s"'
echo "      ssl_certificate: $PEM"
