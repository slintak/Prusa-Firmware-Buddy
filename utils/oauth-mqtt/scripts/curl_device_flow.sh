#!/usr/bin/env bash
set -euo pipefail

# Full OAuth device flow via curl.
# Produces:
#   - ./tmp/device_flow_last.json
#   - ./tmp/device_flow_last.env (DEVICE_CODE/USER_CODE/ACCESS_TOKEN/REFRESH_TOKEN/MQTT_USERNAME)
#
# Usage:
#   ./scripts/curl_device_flow.sh [SN]
#
# Env overrides:
#   OAUTH_BASE_URL   (default: https://10.2.0.248:8443)
#   OAUTH_CA_FILE    (default: ./certs/ca.crt)
#   OAUTH_CLIENT_ID  (default: test-client)
#   OAUTH_SCOPE      (default: mqtt)
#   OAUTH_INSECURE   (default: 0; set 1 to add -k)

SN="${1:-SN123456789}"
BASE_URL="${OAUTH_BASE_URL:-https://10.2.0.248:8443}"
CA_FILE="${OAUTH_CA_FILE:-./certs/ca.crt}"
CLIENT_ID="${OAUTH_CLIENT_ID:-test-client}"
SCOPE="${OAUTH_SCOPE:-mqtt}"
INSECURE="${OAUTH_INSECURE:-0}"

TMP_DIR="./tmp"
mkdir -p "${TMP_DIR}"

curl_opts=(-sS)
if [[ "${INSECURE}" == "1" ]]; then
  curl_opts+=(-k)
else
  curl_opts+=(--cacert "${CA_FILE}")
fi

echo "[1/4] Requesting device code..."
RESP1="$(
  curl "${curl_opts[@]}" -X POST "${BASE_URL}/oauth/device_authorization" \
    -d "client_id=${CLIENT_ID}" \
    -d "scope=${SCOPE}"
)"
echo "${RESP1}" | jq .

DEVICE_CODE="$(echo "${RESP1}" | jq -r '.device_code')"
USER_CODE="$(echo "${RESP1}" | jq -r '.user_code')"

echo "[2/4] Approving user code ${USER_CODE}..."
curl "${curl_opts[@]}" -X POST "${BASE_URL}/oauth/verify" \
  -d "user_code=${USER_CODE}" >/dev/null

echo "[3/4] Polling token endpoint..."
RESP2="$(
  curl "${curl_opts[@]}" -X POST "${BASE_URL}/oauth/token" \
    -d "grant_type=urn:ietf:params:oauth:grant-type:device_code" \
    -d "device_code=${DEVICE_CODE}" \
    -d "client_id=${CLIENT_ID}" \
    -d "sn=${SN}"
)"
echo "${RESP2}" | jq .

ACCESS_TOKEN="$(echo "${RESP2}" | jq -r '.access_token')"
REFRESH_TOKEN="$(echo "${RESP2}" | jq -r '.refresh_token')"

if [[ -z "${ACCESS_TOKEN}" || "${ACCESS_TOKEN}" == "null" ]]; then
  echo "ERROR: access_token missing in response." >&2
  exit 1
fi

echo "[4/4] Decoding JWT payload..."
PAYLOAD_JSON="$(
  python3 - "${ACCESS_TOKEN}" <<'PY'
import base64
import json
import sys
tok = sys.argv[1]
payload = tok.split(".")[1]
payload += "=" * ((4 - len(payload) % 4) % 4)
print(json.dumps(json.loads(base64.urlsafe_b64decode(payload)), indent=2))
PY
)"
echo "${PAYLOAD_JSON}"
MQTT_USERNAME="$(echo "${PAYLOAD_JSON}" | jq -r '.mqtt_username // empty')"

cat > "${TMP_DIR}/device_flow_last.env" <<EOF
SN=${SN}
BASE_URL=${BASE_URL}
CLIENT_ID=${CLIENT_ID}
SCOPE=${SCOPE}
DEVICE_CODE=${DEVICE_CODE}
USER_CODE=${USER_CODE}
ACCESS_TOKEN=${ACCESS_TOKEN}
REFRESH_TOKEN=${REFRESH_TOKEN}
MQTT_USERNAME=${MQTT_USERNAME}
EOF

cat > "${TMP_DIR}/device_flow_last.json" <<EOF
{
  "sn": ${SN@Q},
  "base_url": ${BASE_URL@Q},
  "client_id": ${CLIENT_ID@Q},
  "scope": ${SCOPE@Q},
  "device_code": ${DEVICE_CODE@Q},
  "user_code": ${USER_CODE@Q},
  "access_token": ${ACCESS_TOKEN@Q},
  "refresh_token": ${REFRESH_TOKEN@Q},
  "mqtt_username": ${MQTT_USERNAME@Q}
}
EOF

echo
echo "Saved:"
echo "  ${TMP_DIR}/device_flow_last.env"
echo "  ${TMP_DIR}/device_flow_last.json"
echo
echo "Next:"
echo "  source ${TMP_DIR}/device_flow_last.env"
echo "  ./scripts/curl_refresh_token.sh"
