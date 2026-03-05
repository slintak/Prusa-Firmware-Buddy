import asyncio
import base64
import logging
import os
import secrets
import string
import time
import uuid
from dataclasses import dataclass
from typing import Dict, Optional

import jwt
from fastapi import FastAPI, Form, HTTPException
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse
from jwcrypto import jwk

from oauth_server.config import load_config
from oauth_server.mqtt_provisioning import init_provisioner, get_provisioner

logger = logging.getLogger("oauth_server")
logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"))

app = FastAPI()

CONFIG = load_config()
OAUTH = CONFIG["oauth"]
MQTT = CONFIG["mqtt"]

ISSUER_URL = OAUTH.get("issuer_url") or None
PUBLIC_BASE_URL = OAUTH.get("public_base_url", "http://localhost:8443")
ACCESS_TOKEN_EXPIRE_SECONDS = int(OAUTH.get("access_token_expires_in", 900))
REFRESH_TOKEN_EXPIRE_SECONDS = int(OAUTH.get("refresh_token_expires_in", 86400))
DEVICE_CODE_EXPIRE_SECONDS = int(OAUTH.get("device_code_expires_in", 600))
POLL_INTERVAL_SECONDS = int(OAUTH.get("poll_interval_seconds", 5))
CLEANUP_INTERVAL_SECONDS = int(OAUTH.get("cleanup_interval_seconds", 10))
TOKEN_FORMAT = str(OAUTH.get("token_format", "opaque")).lower()
JWT_PRIVATE_KEY_PATH = OAUTH.get("jwt_private_key_path", "")
JWT_KID = OAUTH.get("jwt_kid", "")
JWT_SUBJECT = str(OAUTH.get("jwt_subject", "sandbox-user"))
JWT_EMAIL = str(OAUTH.get("jwt_email", "sandbox@example.com"))
JWT_ACCOUNT_ID = int(OAUTH.get("jwt_account_id", 1))
JWT_REFRESH_APP = str(OAUTH.get("jwt_refresh_app", "oauth-mqtt-sandbox"))
SN_UUID_MAP = OAUTH.get("sn_uuid_map", {}) or {}

USER_CODE_ALPHABET = string.ascii_uppercase + string.digits


@dataclass
class DeviceState:
    client_id: str
    scope: str
    mqtt_username: Optional[str]
    device_sn: Optional[str]
    device_label: Optional[str]
    created_at: int
    expires_in: int
    interval: int
    approved: bool = False
    last_poll_at: Optional[float] = None


@dataclass
class RefreshState:
    client_id: str
    scope: str
    mqtt_username: Optional[str]
    device_sn: Optional[str]
    device_label: Optional[str]
    created_at: int
    expires_in: Optional[int]


refresh_by_token: Dict[str, RefreshState] = {}


device_by_code: Dict[str, DeviceState] = {}
user_code_map: Dict[str, str] = {}
active_devices: Dict[str, int] = {}
sn_to_uuid_map: Dict[str, str] = {}

_cached_private_key: Optional[str] = None
_cached_kid: Optional[str] = None
_cleanup_task: Optional[asyncio.Task] = None


def _issuer() -> str:
    return ISSUER_URL or PUBLIC_BASE_URL


def _now() -> int:
    return int(time.time())


def _random_user_code(length: int = 8) -> str:
    return "".join(secrets.choice(USER_CODE_ALPHABET) for _ in range(length))


def _random_device_code() -> str:
    return secrets.token_urlsafe(24)


def _load_private_key() -> str:
    global _cached_private_key
    if _cached_private_key:
        return _cached_private_key
    if JWT_PRIVATE_KEY_PATH:
        with open(JWT_PRIVATE_KEY_PATH, "r", encoding="utf-8") as handle:
            _cached_private_key = handle.read()
        return _cached_private_key
    # Sandbox fallback: generate ephemeral RSA key if no key path is configured.
    key = jwk.JWK.generate(kty="RSA", size=2048)
    _cached_private_key = key.export_to_pem(private_key=True, password=None).decode("utf-8")
    logger.warning("TOKEN_FORMAT=jwt with no jwt_private_key_path: using ephemeral in-memory RSA key")
    return _cached_private_key


def _kid_from_pem(pem: str) -> str:
    key = jwk.JWK.from_pem(pem.encode("utf-8"))
    return key.thumbprint()


def _mqtt_username_from_sn(device_sn: Optional[str]) -> Optional[str]:
    if not device_sn:
        return None
    # Explicit map has precedence (useful for fixed sandbox fixtures).
    if device_sn in SN_UUID_MAP:
        return str(SN_UUID_MAP[device_sn])
    if device_sn in sn_to_uuid_map:
        return sn_to_uuid_map[device_sn]
    # Stable deterministic UUID for each SN in sandbox.
    generated = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"sandbox-sn:{device_sn}"))
    sn_to_uuid_map[device_sn] = generated
    return generated


def _jwt_access_token(
    client_id: str,
    scope: str,
    mqtt_username: Optional[str],
    device_sn: Optional[str],
    device_label: Optional[str],
) -> str:
    pem = _load_private_key()
    now = _now()
    payload = {
        "iss": _issuer(),
        "sub": JWT_SUBJECT,
        "aud": f"client:{client_id}",
        "iat": now,
        "exp": now + ACCESS_TOKEN_EXPIRE_SECONDS,
        "jti": str(uuid.uuid4()),
        "scope": scope or "",
        "token_type": "access_token",
        "email": JWT_EMAIL,
        "account_id": JWT_ACCOUNT_ID,
        # Connect identity expected by firmware
        "mqtt_username": mqtt_username,
        "device_id": mqtt_username,
        "device_sn": device_sn,
        "device_label": device_label,
    }
    payload = {k: v for k, v in payload.items() if v is not None}
    kid = JWT_KID or _kid_from_pem(pem)
    return jwt.encode(payload, pem, algorithm="RS256", headers={"kid": kid})


def _opaque_access_token() -> str:
    raw = secrets.token_urlsafe(32)
    return base64.urlsafe_b64encode(raw.encode("utf-8")).decode("utf-8").rstrip("=")


def _issue_access_token(state: DeviceState) -> str:
    if TOKEN_FORMAT == "jwt":
        return _jwt_access_token(
            client_id=state.client_id,
            scope=state.scope,
            mqtt_username=state.mqtt_username,
            device_sn=state.device_sn,
            device_label=state.device_label,
        )
    return _opaque_access_token()


def _jwt_refresh_token(state: DeviceState) -> str:
    pem = _load_private_key()
    now = _now()
    payload = {
        "jti": uuid.uuid4().hex,
        "sub": JWT_SUBJECT,
        "exp": now + REFRESH_TOKEN_EXPIRE_SECONDS if REFRESH_TOKEN_EXPIRE_SECONDS > 0 else None,
        "sid": None,
        "app": JWT_REFRESH_APP,
        "type": "refresh",
        "mqtt_username": state.mqtt_username,
        "device_id": state.mqtt_username,
        "device_sn": state.device_sn,
        "device_label": state.device_label,
    }
    payload = {k: v for k, v in payload.items() if v is not None}
    kid = JWT_KID or _kid_from_pem(pem)
    return jwt.encode(payload, pem, algorithm="RS256", headers={"kid": kid})


def _issue_refresh_token(state: DeviceState) -> str:
    if TOKEN_FORMAT == "jwt":
        return _jwt_refresh_token(state)
    return _opaque_access_token()


def _refresh_access_token(refresh_state: RefreshState) -> str:
    if TOKEN_FORMAT == "jwt":
        return _jwt_access_token(
            client_id=refresh_state.client_id,
            scope=refresh_state.scope,
            mqtt_username=refresh_state.mqtt_username,
            device_sn=refresh_state.device_sn,
            device_label=refresh_state.device_label,
        )
    return _opaque_access_token()


def _record_active_device(mqtt_username: Optional[str], expires_at: int) -> None:
    if not mqtt_username:
        return
    active_devices[mqtt_username] = expires_at


def _verification_uri() -> str:
    return f"{PUBLIC_BASE_URL.rstrip('/')}/oauth/verify"


def _cleanup_expired(device_code: str, state: DeviceState) -> None:
    if _now() <= state.created_at + state.expires_in:
        return
    device_by_code.pop(device_code, None)
    for code, mapped in list(user_code_map.items()):
        if mapped == device_code:
            user_code_map.pop(code, None)


@app.post("/oauth/device_authorization")
async def device_authorization(
    client_id: str = Form(...),
    scope: str = Form(""),
):
    device_code = _random_device_code()
    user_code = _random_user_code()

    state = DeviceState(
        client_id=client_id,
        scope=scope,
        mqtt_username=None,
        device_sn=None,
        device_label=None,
        created_at=_now(),
        expires_in=DEVICE_CODE_EXPIRE_SECONDS,
        interval=POLL_INTERVAL_SECONDS,
        approved=False,
    )
    device_by_code[device_code] = state
    user_code_map[user_code] = device_code

    verification_uri = _verification_uri()
    logger.info("issued device_code=%s user_code=%s", device_code, user_code)

    return {
        "device_code": device_code,
        "user_code": user_code,
        "verification_uri": verification_uri,
        "verification_uri_complete": f"{verification_uri}?user_code={user_code}",
        "expires_in": state.expires_in,
        "interval": state.interval,
    }


@app.post("/oauth/token")
async def token(
    grant_type: str = Form(...),
    device_code: Optional[str] = Form(None),
    refresh_token: Optional[str] = Form(None),
    client_id: Optional[str] = Form(None),
    # SN-like hardware identifier from firmware; used to derive MQTT UUID in sandbox.
    sn: Optional[str] = Form(None),
    device_sn: Optional[str] = Form(None),
    # Legacy input, still accepted for compatibility.
    device_id: Optional[str] = Form(None),
    device_label: Optional[str] = Form(None),
):
    if grant_type == "refresh_token":
        if not refresh_token or not client_id:
            return JSONResponse(status_code=400, content={"error": "invalid_request"})
        refresh_state = refresh_by_token.get(refresh_token)
        if not refresh_state:
            return JSONResponse(status_code=400, content={"error": "invalid_grant"})
        if refresh_state.expires_in is not None and _now() > refresh_state.created_at + refresh_state.expires_in:
            refresh_by_token.pop(refresh_token, None)
            return JSONResponse(status_code=400, content={"error": "invalid_grant"})
        if refresh_state.client_id != client_id:
            return JSONResponse(status_code=400, content={"error": "invalid_client"})

        access_token = _refresh_access_token(refresh_state)
        provisioner = get_provisioner()
        if provisioner and refresh_state.mqtt_username:
            provisioner.provision_device(refresh_state.mqtt_username, access_token)
        _record_active_device(refresh_state.mqtt_username, _now() + ACCESS_TOKEN_EXPIRE_SECONDS)
        return {
            "access_token": access_token,
            "token_type": "Bearer",
            "expires_in": ACCESS_TOKEN_EXPIRE_SECONDS,
            "scope": refresh_state.scope,
        }

    if grant_type != "urn:ietf:params:oauth:grant-type:device_code":
        return JSONResponse(status_code=400, content={"error": "unsupported_grant_type"})

    if not device_code or not client_id or device_code not in device_by_code:
        return JSONResponse(status_code=400, content={"error": "invalid_request"})

    state = device_by_code[device_code]
    _cleanup_expired(device_code, state)

    if device_code not in device_by_code:
        return JSONResponse(status_code=400, content={"error": "expired_token"})

    if state.client_id != client_id:
        return JSONResponse(status_code=400, content={"error": "invalid_client"})

    now = time.time()
    if state.last_poll_at is not None and now - state.last_poll_at < state.interval:
        state.last_poll_at = now
        return JSONResponse(status_code=400, content={"error": "slow_down"})
    state.last_poll_at = now

    incoming_sn = device_sn or sn or device_id
    if state.device_sn is None:
        state.device_sn = incoming_sn or f"SN-{device_code[:8]}"
    if state.mqtt_username is None:
        state.mqtt_username = _mqtt_username_from_sn(state.device_sn)
    state.device_label = state.device_label or device_label

    if not state.approved:
        return JSONResponse(status_code=400, content={"error": "authorization_pending"})

    access_token = _issue_access_token(state)
    refresh_token_value = _issue_refresh_token(state)
    refresh_by_token[refresh_token_value] = RefreshState(
        client_id=state.client_id,
        scope=state.scope,
        mqtt_username=state.mqtt_username,
        device_sn=state.device_sn,
        device_label=state.device_label,
        created_at=_now(),
        expires_in=REFRESH_TOKEN_EXPIRE_SECONDS if REFRESH_TOKEN_EXPIRE_SECONDS > 0 else None,
    )
    provisioner = get_provisioner()
    if provisioner and state.mqtt_username:
        provisioner.provision_device(state.mqtt_username, access_token)
    _record_active_device(state.mqtt_username, _now() + ACCESS_TOKEN_EXPIRE_SECONDS)

    return {
        "access_token": access_token,
        "refresh_token": refresh_token_value,
        "token_type": "Bearer",
        "expires_in": ACCESS_TOKEN_EXPIRE_SECONDS,
        "scope": state.scope,
    }


@app.get("/oauth/verify", response_class=HTMLResponse)
async def verify_page(user_code: Optional[str] = None):
    return _verify_page_html(user_code=user_code, message=None, error=None)


@app.post("/oauth/verify", response_class=HTMLResponse)
async def verify_submit(user_code: str = Form("")):
    if not user_code or user_code not in user_code_map:
        return _verify_page_html(user_code=user_code, message=None, error="Invalid user_code")

    device_code = user_code_map[user_code]
    state = device_by_code.get(device_code)
    if not state:
        return _verify_page_html(user_code=user_code, message=None, error="Device expired")

    state.approved = True
    logger.info("approved user_code=%s device_code=%s mqtt_username=%s", user_code, device_code, state.mqtt_username)
    return _verify_page_html(user_code=user_code, message="Approved", error=None)


@app.get("/healthz")
async def healthz():
    return {"status": "ok"}


@app.get("/")
async def root():
    return PlainTextResponse("oauth-mqtt-sandbox")


@app.on_event("startup")
async def startup() -> None:
    init_provisioner(MQTT)
    provisioner = get_provisioner()
    if provisioner and provisioner.enabled:
        if not MQTT.get("username") or not MQTT.get("password") or MQTT.get("password") == "change-me":
            raise RuntimeError("mqtt.username/mqtt.password must be set in config.yaml")
        provisioner.connect()

    global _cleanup_task
    _cleanup_task = asyncio.create_task(_cleanup_loop())


@app.on_event("shutdown")
async def shutdown() -> None:
    global _cleanup_task
    if _cleanup_task:
        _cleanup_task.cancel()
        _cleanup_task = None
    provisioner = get_provisioner()
    if provisioner:
        provisioner.close()


async def _cleanup_loop() -> None:
    while True:
        now = _now()
        expired = [device_id for device_id, exp in active_devices.items() if exp <= now]
        if expired:
            provisioner = get_provisioner()
            for device_id in expired:
                if provisioner:
                    provisioner.revoke_device(device_id)
                active_devices.pop(device_id, None)
        await asyncio.sleep(CLEANUP_INTERVAL_SECONDS)


def _verify_page_html(user_code: Optional[str], message: Optional[str], error: Optional[str]) -> str:
    user_code = user_code or ""
    message_html = f"<p style='color:green'>{message}</p>" if message else ""
    error_html = f"<p style='color:red'>{error}</p>" if error else ""
    return f"""
    <html>
      <head>
        <title>Device Flow Verify</title>
        <style>
          body {{ font-family: sans-serif; margin: 40px; }}
          input {{ font-size: 18px; padding: 6px; }}
          button {{ font-size: 16px; padding: 6px 10px; }}
        </style>
      </head>
      <body>
        <h2>OAuth Device Flow Verification</h2>
        {message_html}
        {error_html}
        <form method="POST" action="/oauth/verify">
          <label>User Code</label><br />
          <input name="user_code" value="{user_code}" />
          <button type="submit">Approve</button>
        </form>
      </body>
    </html>
    """


def main() -> None:
    import uvicorn

    tls = OAUTH.get("tls", {}) or {}
    cert_file = tls.get("cert_file") or None
    key_file = tls.get("key_file") or None
    ciphers = tls.get("ciphers") or None

    uvicorn.run(
        "oauth_server.main:app",
        host=OAUTH.get("bind_host", "0.0.0.0"),
        port=int(OAUTH.get("bind_port", 8443)),
        log_level=os.getenv("UVICORN_LOG_LEVEL", "info"),
        ssl_certfile=cert_file,
        ssl_keyfile=key_file,
        ssl_ciphers=ciphers,
    )


if __name__ == "__main__":
    main()
