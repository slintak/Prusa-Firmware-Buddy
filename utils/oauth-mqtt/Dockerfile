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

COPY pyproject.toml README.md config.yaml /app/
COPY oauth_server /app/oauth_server
COPY mosquitto /app/mosquitto
COPY scripts /app/scripts

ENV PATH="/opt/venv/bin:${PATH}"
ENV UV_PROJECT_ENVIRONMENT=/opt/venv

RUN uv sync

ENV OAUTH_CONFIG_PATH=/app/config.yaml

EXPOSE 8443 8883

ENTRYPOINT ["tini", "--"]
CMD ["/app/scripts/entrypoint.sh"]
