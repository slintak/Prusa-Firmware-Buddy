FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        mosquitto \
        mosquitto-clients \
        python3 \
        python3-pip \
        python3-venv \
        openssl \
        ca-certificates \
        tini \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir uv

WORKDIR /app

COPY utils/oauth-mqtt/pyproject.toml utils/oauth-mqtt/README.md utils/oauth-mqtt/sandbox.yaml.example /app/
COPY utils/oauth-mqtt/proto_codegen.py /app/
COPY utils/oauth-mqtt/sandbox_config.py /app/
COPY utils/oauth-mqtt/connect_proxy /app/connect_proxy
COPY utils/oauth-mqtt/oauth_server /app/oauth_server
COPY specs/connect2 /app/specs/connect2
COPY utils/oauth-mqtt/mosquitto /app/mosquitto
COPY utils/oauth-mqtt/scripts /app/scripts

ENV PATH="/opt/venv/bin:${PATH}"
ENV UV_PROJECT_ENVIRONMENT=/opt/venv
ENV PYTHONPATH=/app

RUN uv sync

ENV OAUTH_CONFIG_PATH=/app/sandbox.yaml

EXPOSE 8443 8883

ENTRYPOINT ["tini", "--", "/app/scripts/entrypoint.sh"]
CMD ["uv", "run", "oauth-server"]
