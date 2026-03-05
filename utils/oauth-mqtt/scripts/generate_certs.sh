#!/bin/sh
set -e

OUT_DIR=${1:-./certs}
SERVER_CN=${2:-}

if [ -z "$SERVER_CN" ]; then
  echo "Usage: $0 <output_dir> <server_cn_or_ip>" >&2
  echo "Example: $0 ./certs 10.2.0.248" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

CA_KEY="$OUT_DIR/ca.key"
CA_CRT="$OUT_DIR/ca.crt"
SERVER_KEY="$OUT_DIR/server.key"
SERVER_CSR="$OUT_DIR/server.csr"
SERVER_EXT="$OUT_DIR/server.ext"
SERVER_CRT="$OUT_DIR/server.crt"
CA_DER="$OUT_DIR/ca.der"

if [ ! -f "$CA_KEY" ] || [ ! -f "$CA_CRT" ]; then
  openssl ecparam -genkey -name prime256v1 -out "$CA_KEY"
  openssl req -x509 -new -key "$CA_KEY" -sha256 -days 3650 \
    -subj "/C=CZ/O=Dev CA/CN=Dev Mosquitto CA" -out "$CA_CRT"
fi

openssl ecparam -genkey -name prime256v1 -out "$SERVER_KEY"
openssl req -new -key "$SERVER_KEY" -subj "/C=CZ/O=Dev/CN=$SERVER_CN" -out "$SERVER_CSR"

cat > "$SERVER_EXT" <<EOF_EXT
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
EOF_EXT

openssl x509 -req -in "$SERVER_CSR" -CA "$CA_CRT" -CAkey "$CA_KEY" -CAcreateserial \
  -out "$SERVER_CRT" -days 825 -extfile "$SERVER_EXT"

openssl x509 -in "$CA_CRT" -outform der -out "$CA_DER"

echo "Generated certificates in $OUT_DIR"
