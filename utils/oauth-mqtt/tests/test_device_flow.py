import importlib
import os

import jwt
from fastapi.testclient import TestClient


def _make_client(tmp_path, token_format: str = "opaque") -> TestClient:
    config_path = tmp_path / "config.yaml"
    config_path.write_text(
        (
            "oauth:\n"
            f"  token_format: {token_format}\n"
            "  poll_interval_seconds: 0\n"
            "  cleanup_interval_seconds: 1\n"
            "  access_token_expires_in: 300\n"
            "  refresh_token_expires_in: 0\n"
            "mqtt:\n"
            "  enabled: false\n"
        ),
        encoding="utf-8",
    )
    os.environ["OAUTH_CONFIG_PATH"] = str(config_path)
    import oauth_server.main as main

    importlib.reload(main)
    return TestClient(main.app)


def _device_authorize(client: TestClient, client_id: str = "test-client", scope: str = "") -> dict:
    return client.post(
        "/oauth/device_authorization",
        data={"client_id": client_id, "scope": scope},
    ).json()


def _approve_user_code(client: TestClient, user_code: str) -> None:
    client.post("/oauth/verify", data={"user_code": user_code})


def _poll_token(client: TestClient, payload: dict) -> dict:
    return client.post("/oauth/token", data=payload).json()


def test_device_authorization_returns_required_fields(tmp_path):
    client = _make_client(tmp_path)
    data = _device_authorize(client)

    assert "device_code" in data
    assert "user_code" in data
    assert "verification_uri" in data
    assert "verification_uri_complete" in data
    assert "expires_in" in data
    assert "interval" in data


def test_token_polling_pending_then_success(tmp_path):
    client = _make_client(tmp_path, token_format="jwt")
    device = _device_authorize(client, scope="mqtt")
    token_payload = {
        "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
        "device_code": device["device_code"],
        "client_id": "test-client",
        "sn": "SN-TEST-123",
    }

    pending = _poll_token(client, token_payload)
    assert pending == {"error": "authorization_pending"}

    _approve_user_code(client, device["user_code"])

    success = _poll_token(client, token_payload)
    assert "access_token" in success
    assert "refresh_token" in success
    assert success["token_type"] == "Bearer"
    assert success["expires_in"] > 0
    assert success["scope"] == "mqtt"

    claims = jwt.decode(success["access_token"], options={"verify_signature": False})
    assert claims["mqtt_username"]
    assert claims["device_id"] == claims["mqtt_username"]
    assert claims["device_sn"] == "SN-TEST-123"


def test_refresh_token_grant_returns_new_access_token(tmp_path):
    client = _make_client(tmp_path, token_format="jwt")
    device = _device_authorize(client, scope="mqtt")
    token_payload = {
        "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
        "device_code": device["device_code"],
        "client_id": "test-client",
        "sn": "SN-TEST-123",
    }

    _approve_user_code(client, device["user_code"])
    first = _poll_token(client, token_payload)
    refresh_token = first["refresh_token"]

    refreshed = _poll_token(
        client,
        {
            "grant_type": "refresh_token",
            "refresh_token": refresh_token,
            "client_id": "test-client",
        },
    )

    assert "access_token" in refreshed
    assert refreshed["token_type"] == "Bearer"
    assert refreshed["expires_in"] > 0

    first_claims = jwt.decode(first["access_token"], options={"verify_signature": False})
    refreshed_claims = jwt.decode(refreshed["access_token"], options={"verify_signature": False})
    assert refreshed_claims["mqtt_username"] == first_claims["mqtt_username"]
