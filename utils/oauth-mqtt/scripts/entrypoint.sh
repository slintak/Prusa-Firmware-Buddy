#!/bin/sh
set -e

export OAUTH_CONFIG_PATH=${OAUTH_CONFIG_PATH:-/app/sandbox.yaml}
export PYTHONPATH=/app

cd /app

mkdir -p /mosquitto/data /mosquitto/log
chown -R mosquitto:mosquitto /mosquitto/data /mosquitto/log

if [ -f /certs/server.key ]; then
  chown mosquitto:mosquitto /certs/server.key /certs/server.crt /certs/ca.crt 2>/dev/null || true
  chmod 640 /certs/server.key 2>/dev/null || true
  chmod 644 /certs/server.crt /certs/ca.crt 2>/dev/null || true
fi

if [ ! -f /mosquitto/data/dynamic-security.json ]; then
  DYNSEC_ADMIN_PASSWORD=$(/opt/venv/bin/python - <<'PY'
from sandbox_config import load_config
cfg = load_config()
print((cfg.get("mqtt", {}) or {}).get("password", ""))
PY
)
  if [ -z "${DYNSEC_ADMIN_PASSWORD:-}" ] || [ "${DYNSEC_ADMIN_PASSWORD}" = "change-me" ]; then
    echo "mqtt.password must be set in sandbox.yaml on first start" >&2
    exit 1
  fi
  mosquitto_ctrl dynsec init /mosquitto/data/dynamic-security.json admin "$DYNSEC_ADMIN_PASSWORD"
  chown mosquitto:mosquitto /mosquitto/data/dynamic-security.json
  chmod 640 /mosquitto/data/dynamic-security.json
fi

mosquitto -c /app/mosquitto/mosquitto.conf &

/opt/venv/bin/python /app/scripts/dynsec_bootstrap.py

exec "$@"
