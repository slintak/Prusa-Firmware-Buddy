#!/usr/bin/env python3
import base64
import json
import os
import ssl
import sys
import urllib.parse
import urllib.request


def _request_json(url: str, data: dict, ca_file: str | None, insecure: bool) -> dict:
    payload = urllib.parse.urlencode(data).encode("utf-8")
    context = None
    if url.startswith("https://"):
        if not ca_file:
            raise SystemExit("OAUTH_CA_FILE must be set for https URLs")
        context = ssl.create_default_context(cafile=ca_file)
        if insecure:
            context.check_hostname = False
    req = urllib.request.Request(url, data=payload, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, context=context, timeout=10) as resp:
            body = resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8") if exc.fp else ""
        raise SystemExit(f"HTTP {exc.code} for {url}: {body}")
    except urllib.error.URLError as exc:
        raise SystemExit(f"Failed to reach {url}: {exc}")

    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Invalid JSON from {url}: {exc}: {body}")


def _post_form(url: str, data: dict, ca_file: str | None, insecure: bool) -> None:
    payload = urllib.parse.urlencode(data).encode("utf-8")
    context = None
    if url.startswith("https://"):
        if not ca_file:
            raise SystemExit("OAUTH_CA_FILE must be set for https URLs")
        context = ssl.create_default_context(cafile=ca_file)
        if insecure:
            context.check_hostname = False
    req = urllib.request.Request(url, data=payload, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, context=context, timeout=10) as _resp:
            return
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8") if exc.fp else ""
        raise SystemExit(f"HTTP {exc.code} for {url}: {body}")
    except urllib.error.URLError as exc:
        raise SystemExit(f"Failed to reach {url}: {exc}")


def _decode_jwt_payload(token: str) -> dict:
    parts = token.split(".")
    if len(parts) != 3:
        return {}
    payload = parts[1]
    payload += "=" * ((4 - len(payload) % 4) % 4)
    try:
        raw = base64.urlsafe_b64decode(payload.encode("utf-8"))
        data = json.loads(raw.decode("utf-8"))
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit("Usage: device_flow <printer_sn>")

    printer_sn = sys.argv[1]
    base_url = os.getenv("OAUTH_BASE_URL", "https://10.2.0.248:8443").rstrip("/")
    ca_file = os.getenv("OAUTH_CA_FILE", "./certs/ca.crt")
    client_id = os.getenv("OAUTH_CLIENT_ID", "test-client")
    scope = os.getenv("OAUTH_SCOPE", "mqtt")

    insecure = os.getenv("OAUTH_INSECURE", "").lower() in {"1", "true", "yes"}

    device = _request_json(
        f"{base_url}/oauth/device_authorization",
        {"client_id": client_id, "scope": scope},
        ca_file,
        insecure,
    )

    user_code = device["user_code"]
    verification_uri = device["verification_uri"]
    print(f"user_code={user_code}")
    print(f"verification_uri={verification_uri}")

    _post_form(
        f"{base_url}/oauth/verify",
        {"user_code": user_code},
        ca_file,
        insecure,
    )

    tokens = _request_json(
        f"{base_url}/oauth/token",
        {
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "device_code": device["device_code"],
            "client_id": client_id,
            "sn": printer_sn,
        },
        ca_file,
        insecure,
    )

    claims = _decode_jwt_payload(tokens["access_token"])
    print(f"access_token={tokens['access_token']}")
    print(f"refresh_token={tokens['refresh_token']}")
    if claims.get("mqtt_username"):
        print(f"mqtt_username={claims['mqtt_username']}")


if __name__ == "__main__":
    main()
