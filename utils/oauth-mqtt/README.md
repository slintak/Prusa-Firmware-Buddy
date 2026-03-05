# oauth-mqtt-sandbox

**Minimal local sandbox** that bundles:
- an OAuth 2.0 Device Flow server
- a Mosquitto broker
- automatic provisioning so the **access token becomes the MQTT password**

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

2. Configure `config.yaml` (see `config.example.yaml` as template):
- set `mqtt.enabled: true`
- set `mqtt.username/password` (dynsec admin credentials)
- set TLS paths under `oauth.tls` and `mqtt.tls`
- keep `oauth.tls.ciphers: ECDHE-ECDSA-AES128-GCM-SHA256` (matches firmware TLS requirements)

3. Run:

```bash
docker compose up --build
```

## Configuration

Configuration is loaded from `config.yaml` (override with `OAUTH_CONFIG_PATH`).

Key settings (see `config.yaml` for full structure):

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

MQTT provisioning:

- `mqtt.enabled`: Enables MQTT provisioning.
- `mqtt.provisioner`: `mosquitto_ctrl` (default) or `control_topic`.
- `mqtt.host` / `mqtt.port`: Mosquitto address (TLS on 8883).
- `mqtt.username` / `mqtt.password`: Dynsec admin credentials (must match `DYNSEC_ADMIN_PASSWORD`).
- `mqtt.tls.ca_file`: CA bundle for Mosquitto TLS.
- `mqtt.tls.cert_file` / `mqtt.tls.key_file`: Optional client certs for mTLS.
- `mqtt.control_topic`: Default `$CONTROL/dynamic-security/v1`.
- `mqtt.response_topic`: Default `$CONTROL/dynamic-security/v1/response`.
- `mqtt.device_role`: Role assigned to device clients.
- `mqtt.device_role_per_client`: If true, creates per-device roles with `%u` expanded to device id.
- `mqtt.device_acls`: ACLs for device role.
- `mqtt.control_role` / `mqtt.control_acls`: Role for control user.

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

Edit `docker-compose.yml` and `config.yaml` to set credentials, TLS paths, and
ACLs.

## License

GPL-3.0. See `LICENSE`.
