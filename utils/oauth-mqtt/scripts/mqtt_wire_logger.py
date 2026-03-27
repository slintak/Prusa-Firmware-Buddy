#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import logging
import os
import signal
import ssl
import sys
import textwrap
import time
from pathlib import Path
from typing import Any

import paho.mqtt.client as mqtt

from sandbox_config import load_config


LOGGER = logging.getLogger("mqtt_wire_logger")
TOPIC_PREFIX = "v1/devices/printers/"
DEFAULT_TOPIC = "v1/devices/printers/#"
RESET = "\033[0m"

# ANSI background + foreground pairs for easy visual grouping by topic type.
TOPIC_STYLES: dict[str, str] = {
    "cmd": "\033[48;5;214m\033[38;5;16m",  # orange bg, black fg
    "gcode": "\033[48;5;39m\033[38;5;15m",  # blue bg, white fg
    "telemetry": "\033[48;5;28m\033[38;5;15m",  # green bg, white fg
    "event": "\033[48;5;166m\033[38;5;15m",  # dark orange bg, white fg
    "transfer": "\033[48;5;99m\033[38;5;15m",  # purple-ish bg, white fg
    "debug": "\033[48;5;240m\033[38;5;15m",  # gray bg, white fg
    "data": "\033[48;5;172m\033[38;5;16m",  # brown/yellow bg, black fg
}
DEFAULT_STYLE = "\033[48;5;25m\033[38;5;15m"
BLOCK_WIDTH = 100
INNER_WIDTH = BLOCK_WIDTH - 4  # "| " + content + " |"


def _resolve_host_path(configured_path: str) -> str:
    if not configured_path:
        return configured_path

    configured = Path(configured_path)
    if configured.exists():
        return str(configured)

    if configured.is_absolute() and configured.parts[:2] == ("/", "certs"):
        local = Path(__file__).resolve().parents[1] / "certs" / configured.name
        if local.exists():
            return str(local)

    return configured_path


def _parse_topic(topic: str) -> tuple[str, str]:
    if not topic.startswith(TOPIC_PREFIX):
        return "-", "unknown"
    rest = topic[len(TOPIC_PREFIX) :]
    parts = rest.split("/")
    if len(parts) < 2:
        return "-", "unknown"
    device_id = parts[0] or "-"
    command_type = parts[1] or "unknown"
    return device_id, command_type


def _hexdump(payload: bytes, width: int = 16) -> str:
    if not payload:
        return "00000000  |"

    lines: list[str] = []
    for offset in range(0, len(payload), width):
        chunk = payload[offset : offset + width]
        hex_part = " ".join(f"{byte:02x}" for byte in chunk)
        hex_part = hex_part.ljust((width * 3) - 1)
        ascii_part = "".join(chr(byte) if 32 <= byte < 127 else "." for byte in chunk)
        lines.append(f"{offset:08x}  {hex_part}  |{ascii_part}|")
    return "\n".join(lines)


def _fit_line(text: str, width: int) -> str:
    if len(text) <= width:
        return text.ljust(width)
    if width <= 3:
        return text[:width]
    return (text[: width - 3] + "...").ljust(width)


def _block_line(text: str) -> str:
    return f"| {_fit_line(text, INNER_WIDTH)} |"


def _block_wrapped_lines(text: str) -> list[str]:
    wrapped = textwrap.wrap(
        text,
        width=INNER_WIDTH,
        replace_whitespace=False,
        drop_whitespace=False,
        break_long_words=True,
        break_on_hyphens=False,
    )
    if not wrapped:
        return [_block_line("")]
    return [_block_line(line) for line in wrapped]


class MqttWireLogger:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.running = True

        self.client = mqtt.Client(client_id=args.client_id, protocol=mqtt.MQTTv311)
        self.client.username_pw_set(args.username, args.password)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message

        if args.ca_file:
            self.client.tls_set(
                ca_certs=str(args.ca_file),
                certfile=None,
                keyfile=None,
                tls_version=ssl.PROTOCOL_TLS_CLIENT,
            )
            if args.insecure:
                self.client.tls_insecure_set(True)

    def _on_connect(self, client: mqtt.Client, _userdata: object, _flags: dict[str, int], rc: int) -> None:
        if rc != 0:
            LOGGER.error("MQTT connect failed rc=%s", rc)
            self.running = False
            return
        client.subscribe(self.args.topic, qos=1)
        LOGGER.info(
            "Connected to MQTT %s:%d as %s, subscribed %s",
            self.args.host,
            self.args.port,
            self.args.username,
            self.args.topic,
        )

    def _on_disconnect(self, _client: mqtt.Client, _userdata: object, rc: int) -> None:
        LOGGER.warning("MQTT disconnected rc=%s", rc)
        if rc != 0 and self.running:
            LOGGER.info("Trying to reconnect...")

    def _on_message(self, _client: mqtt.Client, _userdata: object, message: mqtt.MQTTMessage) -> None:
        now_iso = dt.datetime.now(dt.timezone.utc).astimezone().isoformat(timespec="milliseconds")
        topic = message.topic or ""
        payload = bytes(message.payload or b"")

        device_id, command_type = _parse_topic(topic)
        style = TOPIC_STYLES.get(command_type, DEFAULT_STYLE)
        header = f"{now_iso}  device_id={device_id}  type={command_type}  len={len(payload)}"
        border = "+" + ("-" * (BLOCK_WIDTH - 2)) + "+"

        print(f"{style}{_fit_line(header, BLOCK_WIDTH)}{RESET}")
        print(border)
        for line in _block_wrapped_lines(f"topic={topic}"):
            print(line)
        print(_block_line(""))
        for dump_line in _hexdump(payload).splitlines():
            print(_block_line(dump_line))
        print(border)
        print("")
        sys.stdout.flush()

    def run(self) -> None:
        self.client.connect(self.args.host, self.args.port, keepalive=self.args.keepalive)
        self.client.loop_start()
        while self.running:
            time.sleep(0.2)

    def stop(self) -> None:
        self.running = False
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass


def _build_parser(defaults: dict[str, Any]) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Log raw MQTT traffic for v1/devices/printers/# with colored topic groups and hexdump payloads."
    )
    parser.add_argument("--host", default=defaults["host"], help="MQTT host")
    parser.add_argument("--port", type=int, default=defaults["port"], help="MQTT port")
    parser.add_argument("--username", default=defaults["username"], help="MQTT username")
    parser.add_argument("--password", default=defaults["password"], help="MQTT password")
    parser.add_argument("--client-id", default=defaults["client_id"], help="MQTT client id")
    parser.add_argument("--keepalive", type=int, default=defaults["keepalive"], help="MQTT keepalive seconds")
    parser.add_argument("--topic", default=DEFAULT_TOPIC, help="MQTT topic filter")
    parser.add_argument("--ca-file", default=defaults["ca_file"], help="CA file path for TLS")
    parser.add_argument("--insecure", action="store_true", default=defaults["insecure"], help="Disable TLS hostname check")
    return parser


def _default_args_from_config() -> dict[str, Any]:
    config = load_config()
    mqtt_cfg = config.get("mqtt", {}) if isinstance(config, dict) else {}
    tls_cfg = mqtt_cfg.get("tls", {}) if isinstance(mqtt_cfg, dict) else {}
    return {
        "host": os.getenv("MOCK_MQTT_HOST", str(mqtt_cfg.get("host", "127.0.0.1"))),
        "port": int(os.getenv("MOCK_MQTT_PORT", str(mqtt_cfg.get("port", 8883)))),
        "username": str(mqtt_cfg.get("username", "admin")),
        "password": str(mqtt_cfg.get("password", "")),
        "client_id": "mqtt-wire-logger",
        "keepalive": int(mqtt_cfg.get("keepalive", 30)),
        "ca_file": _resolve_host_path(str(tls_cfg.get("ca_file", ""))),
        "insecure": bool(tls_cfg.get("insecure", False)),
    }


def main() -> None:
    defaults = _default_args_from_config()
    parser = _build_parser(defaults)
    args = parser.parse_args()
    args.ca_file = _resolve_host_path(str(args.ca_file))

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    app = MqttWireLogger(args)

    def _handle_signal(_signum: int, _frame: object) -> None:
        app.stop()
        raise SystemExit(0)

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)
    app.run()


if __name__ == "__main__":
    main()
