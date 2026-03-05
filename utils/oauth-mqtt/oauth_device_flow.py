# mock_oauth_device_flow.py
import time
import uuid
from dataclasses import dataclass
from typing import Dict, Optional
from urllib.parse import parse_qs

import jwt
from fastapi import FastAPI, Form, Request, HTTPException
from fastapi.responses import HTMLResponse, JSONResponse

from jwcrypto import jwk

app = FastAPI()

# === Konfigurace (uprav podle potřeby) ===
ISSUER_URL = "http://localhost:8085/realms/farm-mode"
ACCESS_TOKEN_EXPIRE_SECONDS = 300
REFRESH_TOKEN_EXPIRE_SECONDS = 3600

# RSA private key PEM (pro dev může být i “test” klíč, ale ať odpovídá tomu co ověřuje backend)
RSA_PRIVATE_KEY_PEM = open("dev_private_key.pem", "r", encoding="utf-8").read()

# “uživatel” (simulace)
USER = {"id": 1, "pk": 1, "email": "user@example.com"}

# === In-memory stav device flow ===
@dataclass
class DeviceState:
    client_id: str
    scope: str
    device_id: Optional[str]
    device_label: Optional[str]
    created_at: int
    expires_in: int
    interval: int
    approved: bool = False

device_by_code: Dict[str, DeviceState] = {}
user_code_map: Dict[str, str] = {}  # user_code -> device_code


def _kid_from_pem(pem: str) -> str:
    key = jwk.JWK.from_pem(pem.encode("utf-8"))
    return key.thumbprint()


def _jwt_access_token(client_id: str, scope: str, device_id: Optional[str], device_label: Optional[str]) -> str:
    now = int(time.time())
    payload = {
        "iss": ISSUER_URL,
        "sub": str(USER["pk"]),
        "aud": f"client:{client_id}",
        "iat": now,
        "exp": now + ACCESS_TOKEN_EXPIRE_SECONDS,
        "jti": str(uuid.uuid4()),
        "scope": scope or "",
        "token_type": "access_token",
        "email": USER["email"],
        "account_id": USER["id"],
        "device_id": device_id,
        "device_label": device_label,
    }
    payload = {k: v for k, v in payload.items() if v is not None}
    headers = {"kid": _kid_from_pem(RSA_PRIVATE_KEY_PEM)}
    return jwt.encode(payload, RSA_PRIVATE_KEY_PEM, algorithm="RS256", headers=headers)


def _jwt_refresh_token(client_id: str, device_id: Optional[str], device_label: Optional[str]) -> str:
    now = int(time.time())
    payload = {
        "jti": uuid.uuid4().hex,
        "sub": str(USER["pk"]),
        "exp": now + REFRESH_TOKEN_EXPIRE_SECONDS,
        "sid": None,
        "app": "test-app",
        "type": "refresh",
        "device_id": device_id,
        "device_label": device_label,
    }
    payload = {k: v for k, v in payload.items() if v is not None}
    headers = {"kid": _kid_from_pem(RSA_PRIVATE_KEY_PEM)}
    return jwt.encode(payload, RSA_PRIVATE_KEY_PEM, algorithm="RS256", headers=headers)


@app.post("/auth/device")
async def device_authorize(
    client_id: str = Form(...),
    scope: str = Form("openid"),
):
    device_code = uuid.uuid4().hex
    user_code = uuid.uuid4().hex[:8].upper()

    st = DeviceState(
        client_id=client_id,
        scope=scope,
        device_id=None,
        device_label=None,
        created_at=int(time.time()),
        expires_in=600,
        interval=2,
        approved=False,
    )
    device_by_code[device_code] = st
    user_code_map[user_code] = device_code

    return {
        "device_code": device_code,
        "user_code": user_code,
        "verification_uri": "http://localhost:8085/verify",
        "verification_uri_complete": f"http://localhost:8085/verify?user_code={user_code}",
        "expires_in": st.expires_in,
        "interval": st.interval,
    }


@app.get("/verify", response_class=HTMLResponse)
async def verify_page(user_code: Optional[str] = None):
    if not user_code:
        return """
        <html><body>
        <h3>Device Flow Verify</h3>
        <p>Append ?user_code=XXXX</p>
        </body></html>
        """
    if user_code not in user_code_map:
        return "<html><body><h3>Invalid code</h3></body></html>"

    device_code = user_code_map[user_code]
    st = device_by_code[device_code]
    st.approved = True
    return f"""
    <html><body>
    <h3>Approved</h3>
    <p>user_code: {user_code}</p>
    <p>device_code: {device_code}</p>
    </body></html>
    """


@app.post("/token")
async def token(request: Request):
    # očekáváme x-www-form-urlencoded
    raw = (await request.body()).decode("utf-8")
    kv = {k: v[0] for k, v in parse_qs(raw, keep_blank_values=True).items()}

    grant_type = kv.get("grant_type")
    client_id = kv.get("client_id")
    device_code = kv.get("device_code")

    # device_id/device_label mohou přijít sem (podle vašeho kódu)
    device_id = kv.get("device_id")
    device_label = kv.get("device_label")

    if grant_type != "urn:ietf:params:oauth:grant-type:device_code":
        raise HTTPException(status_code=400, detail="unsupported_grant_type")

    if not client_id or not device_code or device_code not in device_by_code:
        return JSONResponse(
            status_code=400,
            content={"error": "invalid_request"},
        )

    st = device_by_code[device_code]
    # basic checks
    if int(time.time()) > st.created_at + st.expires_in:
        return JSONResponse(status_code=400, content={"error": "expired_token"})

    # “zapamatuj” device info do state (ať se propsne do JWT)
    st.device_id = st.device_id or device_id
    st.device_label = st.device_label or device_label

    if not st.approved:
        return JSONResponse(status_code=400, content={"error": "authorization_pending"})

    access = _jwt_access_token(st.client_id, st.scope, st.device_id, st.device_label)
    refresh = _jwt_refresh_token(st.client_id, st.device_id, st.device_label)

    return {
        "access_token": access,
        "token_type": "Bearer",
        "expires_in": ACCESS_TOKEN_EXPIRE_SECONDS,
        "refresh_token": refresh,
        "scope": st.scope,
    }


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8085)
