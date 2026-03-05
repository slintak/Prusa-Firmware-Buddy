import os
from copy import deepcopy
from typing import Any, Dict

import yaml


DEFAULT_CONFIG: Dict[str, Any] = {
    "oauth": {
        "public_base_url": "http://localhost:8443",
        "issuer_url": "",
        "token_format": "opaque",
        "access_token_expires_in": 900,
        # <= 0 means no fixed expiry (valid until server-side revocation).
        "refresh_token_expires_in": 0,
        "device_code_expires_in": 600,
        "poll_interval_seconds": 2,
        "cleanup_interval_seconds": 10,
        "keepalive_timeout_seconds": 30,
        "jwt_private_key_path": "",
        "jwt_kid": "",
        "jwt_subject": "sandbox-user",
        "jwt_email": "sandbox@example.com",
        "jwt_account_id": 1,
        "jwt_refresh_app": "oauth-mqtt-sandbox",
        # Optional explicit fixtures: {"SN123": "3d6d4bb7-6ab3-4ec7-b0f5-574ad4b4ad45"}
        "sn_uuid_map": {},
        "bind_host": "0.0.0.0",
        "bind_port": 8443,
        "tls": {
            "ca_file": "",
            "cert_file": "",
            "key_file": "",
            # Keep aligned with firmware TLS requirements.
            "ciphers": "ECDHE-ECDSA-AES128-GCM-SHA256",
        },
    },
    "mqtt": {
        "enabled": False,
        "provisioner": "mosquitto_ctrl",
        "host": "localhost",
        "port": 8883,
        "username": "oauth-control",
        "password": "change-me",
        "client_id": "oauth-control-client",
        "keepalive": 30,
        "tls": {
            "ca_file": "",
            "cert_file": "",
            "key_file": "",
            "insecure": False,
        },
        "control_topic": "$CONTROL/dynamic-security/v1",
        "response_topic": "$CONTROL/dynamic-security/v1/response",
        "device_role": "device-role",
        "device_role_per_client": True,
        "device_acls": [
            {"acltype": "publishClientSend", "topic": "devices/%u/#", "allow": True},
            {"acltype": "publishClientSend", "topic": "v1/devices/printers/%u/#", "allow": True},
            {"acltype": "subscribeLiteral", "topic": "devices/%u/#", "allow": True},
            {"acltype": "subscribePattern", "topic": "v1/devices/printers/%u/#", "allow": True},
            {"acltype": "publishClientReceive", "topic": "devices/%u/#", "allow": True},
            {"acltype": "publishClientReceive", "topic": "v1/devices/printers/%u/#", "allow": True},
        ],
        "control_role": "oauth-control",
        "control_acls": [
            {"acltype": "publishClientSend", "topic": "$CONTROL/dynamic-security/#", "allow": True},
            {"acltype": "subscribePattern", "topic": "$CONTROL/dynamic-security/#", "allow": True},
        ],
    },
}


def load_config() -> Dict[str, Any]:
    path = os.getenv("OAUTH_CONFIG_PATH", "config.yaml")
    config = deepcopy(DEFAULT_CONFIG)

    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as handle:
            data = yaml.safe_load(handle) or {}
        _deep_update(config, data)

    return config


def _deep_update(target: Dict[str, Any], source: Dict[str, Any]) -> None:
    for key, value in source.items():
        if isinstance(value, dict) and isinstance(target.get(key), dict):
            _deep_update(target[key], value)
        else:
            target[key] = value
