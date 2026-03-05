#!/usr/bin/env bash
set -euo pipefail

# Refresh access token via curl using REFRESH_TOKEN from env or tmp/device_flow_last.env.
#
# Usage:
#   ./scripts/curl_refresh_token.sh
#
# Env overrides:
#   OAUTH_BASE_URL   (default: https://10.2.0.248:8443)
#   OAUTH_CA_FILE    (default: ./certs/ca.crt)
#   OAUTH_CLIENT_ID  (default: test-client)
#   OAUTH_INSECURE   (default: 0; set 1 to add -k)
#   REFRESH_TOKEN    (optional if ./tmp/device_flow_last.env exists)

BASE_URL="${OAUTH_BASE_URL:-https://10.2.0.248:8443}"
CA_FILE="${OAUTH_CA_FILE:-./certs/ca.crt}"
CLIENT_ID="${OAUTH_CLIENT_ID:-test-client}"
INSECURE="${OAUTH_INSECURE:-0}"

if [[ -z "${REFRESH_TOKEN:-}" && -f "./tmp/device_flow_last.env" ]]; then
  # shellcheck disable=SC1091
  source ./tmp/device_flow_last.env
fi

if [[ -z "${REFRESH_TOKEN:-}" ]]; then
  echo "ERROR: REFRESH_TOKEN not set and ./tmp/device_flow_last.env not found." >&2
  exit 1
fi

curl_opts=(-sS)
if [[ "${INSECURE}" == "1" ]]; then
  curl_opts+=(-k)
else
  curl_opts+=(--cacert "${CA_FILE}")
fi

echo "Refreshing token..."
RESP="$(
  curl "${curl_opts[@]}" -X POST "${BASE_URL}/oauth/token" \
    -d "grant_type=refresh_token" \
    -d "refresh_token=${REFRESH_TOKEN}" \
    -d "client_id=${CLIENT_ID}"
)"
echo "${RESP}" | jq .

ACCESS_TOKEN_NEW="$(echo "${RESP}" | jq -r '.access_token')"
if [[ -z "${ACCESS_TOKEN_NEW}" || "${ACCESS_TOKEN_NEW}" == "null" ]]; then
  echo "ERROR: access_token missing in refresh response." >&2
  exit 1
fi

echo
echo "Decoded JWT payload:"
python3 - "${ACCESS_TOKEN_NEW}" <<'PY'
import base64
import json
import sys
tok = sys.argv[1]
payload = tok.split(".")[1]
payload += "=" * ((4 - len(payload) % 4) % 4)
print(json.dumps(json.loads(base64.urlsafe_b64decode(payload)), indent=2))
PY
