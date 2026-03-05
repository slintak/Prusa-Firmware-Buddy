#!/bin/sh
set -e

export OAUTH_CONFIG_PATH=${OAUTH_CONFIG_PATH:-/app/config.yaml}
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
  if [ -z "${DYNSEC_ADMIN_PASSWORD:-}" ]; then
    echo "DYNSEC_ADMIN_PASSWORD must be set on first start" >&2
    exit 1
  fi
  mosquitto_ctrl dynsec init /mosquitto/data/dynamic-security.json admin "$DYNSEC_ADMIN_PASSWORD"
fi

mosquitto -c /app/mosquitto/mosquitto.conf &

/opt/venv/bin/python /app/scripts/dynsec_bootstrap.py

exec /opt/venv/bin/uv run server
