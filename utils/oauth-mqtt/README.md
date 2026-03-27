# connect2-sandbox

**Minimal local sandbox** that bundles:
- an OAuth 2.0 Device Flow server
- a Mosquitto broker
- automatic provisioning so the **access token becomes the MQTT password**

The CONNECT2 proxy exists in this repo, but it runs as a **separate process**
outside Docker Compose.

This exists to make firmware and integration development easy. It is **not production-ready**,
**not security-hardened**, and **not intended for production use**.

## Why This Exists

We need a small, reproducible sandbox where devices can:
1. perform OAuth 2.0 Device Authorization Grant (RFC 8628)
2. receive an access token
3. use that access token as the MQTT password

This is only a dev aid. Use a real OAuth/OIDC provider and hardened broker in production.

## Features

- OAuth 2.0 Device Flow endpoints
- Token polling with `authorization_pending`, `slow_down`, `expired_token`
- Refresh token support
- Mosquitto Dynamic Security provisioning
- TLS-only MQTT listener (no plaintext)
- Self-signed CA + server cert generator (ECDSA, TLS 1.2 cipher pinned)
- Single Docker image or Docker Compose

## Quick Start

1. Generate certs (CA + server cert, CN = IP, no SAN):

```bash
./scripts/generate_certs.sh ./certs 10.2.0.248
```

2. Prepare `sandbox.yaml` from `sandbox.yaml.example`:

```bash
cp sandbox.yaml.example sandbox.yaml
```

3. Configure `sandbox.yaml`:
- set `proxy.upstream.url`
- set `mqtt.password` to the dynsec admin password you want the broker initialized with
- set `proxy.mqtt.password` to the same value
- add as many printers as needed under `printers:`
- OAuth only authorizes configured serial numbers
- proxy configuration is still kept in the same file (for `uv run connect-proxy`)
- set TLS paths under `oauth.tls` and `mqtt.tls`
- keep `oauth.tls.ciphers: ECDHE-ECDSA-AES128-GCM-SHA256` (matches firmware TLS requirements)

4. Run sandbox services:

```bash
docker compose up --build
```

5. Run proxy separately (optional):

```bash
uv run connect-proxy
```

Broker persistence is disabled in this sandbox on purpose. Each restart starts
with a clean MQTT state and a fresh dynsec database, which is more convenient
for iterative protocol bring-up.

## Configuration

Configuration is loaded from `sandbox.yaml` (override with `OAUTH_CONFIG_PATH`).

Key settings (see `sandbox.yaml.example` for full structure):

- `oauth.public_base_url`: Base URL used in `verification_uri`.
- `oauth.issuer_url`: Issuer for JWT tokens.
- `oauth.token_format`: `opaque` or `jwt`.
- `oauth.access_token_expires_in`: Access token TTL in seconds.
- `oauth.refresh_token_expires_in`: Refresh token TTL in seconds. `<= 0` means no fixed expiry (revocation-only).
- `oauth.device_code_expires_in`: Device code TTL in seconds.
- `oauth.poll_interval_seconds`: Minimum polling interval for `/oauth/token`.
- `oauth.cleanup_interval_seconds`: How often to revoke expired MQTT credentials.
- `oauth.jwt_private_key_path`: Optional PEM private key for RS256. If empty, sandbox generates in-memory RSA key.
- `oauth.sn_uuid_map`: Optional fixed map `SN -> UUID` for deterministic sandbox fixtures.
- `oauth.bind_host` / `oauth.bind_port`: Bind address and port.
- `oauth.tls.*`: TLS cert/key for the OAuth server (optional). If set, OAuth uses HTTPS.
  - `oauth.tls.ciphers`: OpenSSL cipher string for OAuth HTTPS listener.
- `printers[]`: Authoritative list of allowed printers. Each item contains:
  - `serial_number`
  - `device_id`
  - `connect_token`
  - `connect_fingerprint`
  - `printer_model_id`
  - `firmware_version`

MQTT provisioning:

- `mqtt.enabled`: Enables MQTT provisioning.
- `mqtt.provisioner`: `mosquitto_ctrl` (default) or `control_topic`.
- `mqtt.host` / `mqtt.port`: Mosquitto address (TLS on 8883).
- `mqtt.username` / `mqtt.password`: Dynsec admin credentials used to initialize and manage Mosquitto dynsec.
- `mqtt.tls.ca_file`: CA bundle for Mosquitto TLS.
- `mqtt.tls.cert_file` / `mqtt.tls.key_file`: Optional client certs for mTLS.
- `mqtt.control_topic`: Default `$CONTROL/dynamic-security/v1`.
- `mqtt.response_topic`: Default `$CONTROL/dynamic-security/v1/response`.
- `mqtt.device_role`: Role assigned to device clients.
- `mqtt.device_role_per_client`: If true, creates per-device roles with `%u` expanded to device id.
- `mqtt.device_acls`: ACLs for device role.
- `mqtt.control_role` / `mqtt.control_acls`: Role for control user.

Proxy (separate process):

- `proxy.enabled`: Enables the CONNECT2 bridge process.
- `proxy.mqtt.*`: MQTT connection used by the proxy to subscribe to printer
  event topics and publish command topics.
- `proxy.mqtt.online_topic`: Online/LWT topic used to create and tear down per-printer websocket sessions.
- `proxy.upstream.url`: Production Connect WebSocket endpoint.
- `proxy.upstream.insecure`: Disables TLS verification for the upstream websocket when needed for debugging.
- Production Connect token is configured per printer in `printers[].connect_token`.
- The proxy keeps a websocket pool keyed by configured printer UUID (`device_id`) when run via `uv run connect-proxy`.

MQTT topic convention used by proxy and firmware/mock:

- online/LWT: `v1/devices/printers/<device_id>/data/online`
- printer events to proxy: `v1/devices/printers/<device_id>/event`
- proxy/server commands to printer: `v1/devices/printers/<device_id>/cmd`

Legacy Connect websocket handshake used by the proxy:

- URL: typically `wss://connect.prusa3d.com/p/ws`
- header `Token: <printer connect token>`
- header `Fingerprint: <printers[].connect_fingerprint>`
- websocket subprotocol: `prusa-connect`
- header `User-Agent-Printer: <printers[].printer_model_id>`
- header `User-Agent-Version: <printers[].firmware_version>`

Header names and the websocket subprotocol are fixed by the backend contract.
The `User-Agent-*` values come from `printers[]` so the proxy can mirror the
real printer identity exactly.

Sandbox proxy fingerprint rule:

- the proxy uses the explicit per-printer `connect_fingerprint`
- this must match the 16-character Buddy fingerprint header emitted by firmware

## OAuth Device Flow (curl)

1. Request device code:

```bash
curl -s -X POST https://10.2.0.248:8443/oauth/device_authorization \
  --cacert ./certs/ca.crt \
  -d client_id=test-client \
  -d scope=mqtt
```

2. Approve user code:

```bash
curl -s -X POST https://10.2.0.248:8443/oauth/verify \
  --cacert ./certs/ca.crt \
  -d user_code=PASTE_USER_CODE
```

3. Poll token endpoint:

```bash
curl -s -X POST https://10.2.0.248:8443/oauth/token \
  --cacert ./certs/ca.crt \
  -d grant_type=urn:ietf:params:oauth:grant-type:device_code \
  -d device_code=PASTE_DEVICE_CODE \
  -d client_id=test-client \
  -d sn=SN123456789
```

4. Refresh access token:

```bash
curl -s -X POST https://10.2.0.248:8443/oauth/token \
  --cacert ./certs/ca.crt \
  -d grant_type=refresh_token \
  -d refresh_token=PASTE_REFRESH_TOKEN \
  -d client_id=test-client
```

Notes:
- `expires_in` applies to the access token only.
- refresh token expiry is server-side policy and not returned (RFC 6749/8628).
- In JWT mode sandbox derives deterministic `mqtt_username` UUID from SN and stores it in JWT claims (`mqtt_username`, `device_id`).

Inspect access JWT quickly:

```bash
python3 - <<'PY'
import base64, json, os
tok = os.environ["ACCESS_TOKEN"]
payload = tok.split(".")[1]
payload += "=" * ((4 - len(payload) % 4) % 4)
print(json.dumps(json.loads(base64.urlsafe_b64decode(payload)), indent=2))
PY
```

## Device Flow Script

Run full device flow and print tokens:

```bash
OAUTH_INSECURE=1 uv run device_flow device-123
```

Optional environment variables:
- `OAUTH_BASE_URL` (default `https://10.2.0.248:8443`)
- `OAUTH_CA_FILE` (default `./certs/ca.crt`)
- `OAUTH_CLIENT_ID` (default `test-client`)
- `OAUTH_SCOPE` (default `mqtt`)
- `OAUTH_INSECURE` set to `1` to disable hostname verification for CN-only certs

## Mock Printer

For proxy bring-up without firmware, use the built-in printer simulator:

```bash
uv run mock-printer SN123456789
```

What it does:
- uses sandbox OAuth device flow with the configured printer serial number
- receives an access token and MQTT username/device id
- connects to MQTT like firmware
- publishes `online=1`
- publishes initial `INFO` and `STATE_CHANGED`
- subscribes to `.../cmd`
- handles full `command.proto` surface used by CONNECT2 mock flow, including:
  - info/state/job/file/transfer queries
  - print controls (`START/PAUSE/RESUME/STOP`, ready/idle/reset)
  - filesystem ops (`CREATE_FOLDER`, `DELETE_FILE`, `DELETE_FOLDER`)
  - transfer commands (`START_*_DOWNLOAD`, `STOP_TRANSFER`)
  - dialog/set-value/cancel-object/token commands
- emits matching protobuf events (`INFO`, `JOB_INFO`, `FILE_INFO`, `FILE_CHANGED`,
  `TRANSFER_INFO`, `TRANSFER_*`, `CANCELABLE_CHANGED`, `STATE_CHANGED`,
  `FINISHED`, `REJECTED`, `FAILED`)

Useful overrides when running on the host:
- `OAUTH_BASE_URL` default `https://127.0.0.1:8443`
- `OAUTH_CA_FILE` default `./certs/ca.crt`
- `OAUTH_INSECURE=1`
- `MOCK_MQTT_HOST` default `127.0.0.1`
- `MOCK_MQTT_PORT` default `8883`

## Mock Server

Interactive MQTT+protobuf server-side mock:

```bash
uv run mock-server --password change-me-admin --ca-file certs/ca.crt --insecure --storage-root /tmp/mocksrv
```

- left pane: RX/TX/ACK traffic (wrapped + colored)
- right pane: selected printer state, telemetry, job/transfer projection, quick command help
- supports interactive command palette for full command surface
- mirrors file metadata under `--storage-root/<device_id>/files-index.json`

One-shot mode (useful for scripting and dev):

```bash
uv run mock-server \
  --password change-me-admin \
  --ca-file certs/ca.crt \
  --insecure \
  --storage-root /tmp/mocksrv \
  --command "select d77ff8ee-58f8-47ba-8638-ae7b4391a470" \
  --command "info"
```

## Curl Test Scripts

For repeatable console testing, use these shell scripts:

```bash
# Full flow: device_authorization -> verify -> token, plus JWT decode
./scripts/curl_device_flow.sh SN123456789

# Refresh using token saved by previous script
./scripts/curl_refresh_token.sh
```

`curl_device_flow.sh` stores outputs in:
- `./tmp/device_flow_last.env`
- `./tmp/device_flow_last.json`

You can source the env file and inspect values:

```bash
source ./tmp/device_flow_last.env
echo "$MQTT_USERNAME"
```

## MQTT With Access Token

Use the access token as the MQTT password. MQTT username must match JWT claim `mqtt_username`:

```bash
mosquitto_pub \
  -h 10.2.0.248 -p 8883 \
  --cafile ./certs/ca.crt \
  --tls-version tlsv1.2 \
  --ciphers ECDHE-ECDSA-AES128-GCM-SHA256 \
  -u '<MQTT_USERNAME_FROM_JWT>' -P '<ACCESS_TOKEN>' \
  -t 'devices/<MQTT_USERNAME_FROM_JWT>/hello' \
  -m 'hello world'
```

## Mosquitto Dynamic Security

By default, provisioning uses `mosquitto_ctrl` (dynsec CLI). You can switch to
`control_topic` mode via config.

On startup, `scripts/dynsec_bootstrap.py` connects as dynsec admin and creates:
- control role
- device role
- control user

The `dynamic-security.json` file is persisted in `/mosquitto/data`.

Debug subscribe (admin, all topics incl. control):

```bash
mosquitto_sub \
  -h 10.2.0.248 -p 8883 \
  --cafile ./certs/ca.crt \
  --tls-version tlsv1.2 \
  --ciphers ECDHE-ECDSA-AES128-GCM-SHA256 \
  -u admin -P '<DYNSEC_ADMIN_PASSWORD>' \
  -t '#'
```

## TLS Certificates

This sandbox was built for internal needs that require:
- IP address in CN (no SAN)
- TLS 1.2 only
- a single pinned cipher: `ECDHE-ECDSA-AES128-GCM-SHA256`

If you need different TLS settings or SAN-enabled certs, edit `mosquitto.conf`
and regenerate certs accordingly.

Python HTTPS clients reject CN-only certs by default. For local testing, set
`OAUTH_INSECURE=1`, or use a separate OAuth cert with SAN for strict verification.

## Docker

Build:

```bash
docker build -t oauth-mqtt-sandbox .
```

Run:

```bash
docker run --rm -p 8883:8883 -p 8443:8443 \
  -v $(pwd)/certs:/certs \
  -e DYNSEC_ADMIN_PASSWORD=change-me-admin \
  oauth-mqtt-sandbox:latest
```

## Docker Compose

```bash
docker compose up --build
```

Edit `docker-compose.yml` and `sandbox.yaml` to set credentials, TLS paths, and
ACLs.

## License

GPL-3.0. See `LICENSE`.
