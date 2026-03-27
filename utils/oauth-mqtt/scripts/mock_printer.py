#!/usr/bin/env python3
from __future__ import annotations

import json
import logging
import os
import signal
import ssl
import sys
import time
import zlib
from argparse import ArgumentParser
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import paho.mqtt.client as mqtt

from proto_codegen import ensure_generated, output_dir
from sandbox_config import configured_printers, load_config
from scripts.device_flow import _decode_jwt_payload, _post_form, _request_json


ensure_generated()
generated_dir = output_dir()
if str(generated_dir) not in sys.path:
    sys.path.insert(0, str(generated_dir))

import command_pb2  # type: ignore  # noqa: E402
import debug_pb2  # type: ignore  # noqa: E402
import event_pb2  # type: ignore  # noqa: E402
import gcode_pb2  # type: ignore  # noqa: E402
import telemetry_pb2  # type: ignore  # noqa: E402
import transfer_pb2  # type: ignore  # noqa: E402


LOGGER = logging.getLogger("mock_printer")


@dataclass(slots=True)
class PrinterIdentity:
    serial_number: str
    device_id: str
    connect_token: str
    connect_fingerprint: str = ""
    printer_model_id: str = ""
    firmware_version: str = ""


@dataclass(slots=True)
class TransferSession:
    start_cmd_id: int
    path: str
    transfer_type: int
    size: int
    transferred: int = 0
    time_transferring: int = 0
    stopped: bool = False
    expects_chunks: bool = False
    wait_for_chunks: bool = False
    started_at: float = 0.0

    @property
    def progress(self) -> float:
        if self.size <= 0:
            return 0.0
        return max(0.0, min(100.0, (self.transferred * 100.0) / float(self.size)))

    @property
    def time_remaining(self) -> int:
        if self.transferred <= 0 or self.size <= self.transferred:
            return 0
        # naive estimate for mock usability
        return int(((self.size - self.transferred) / max(self.transferred, 1)) * self.time_transferring)


class MockPrinter:
    """Simple CONNECT2 printer simulator for proxy bring-up.

    The goal is not to emulate firmware internals precisely. The goal is to
    produce the same MQTT-level behavior the proxy expects:
    - authenticate via sandbox OAuth device flow
    - connect to MQTT with mqtt_username/device_id and access token
    - publish online state
    - publish protobuf events on the printer event topic
    - receive protobuf commands on the printer command topic
    - answer with small, deterministic fake payloads
    """

    ALLOWED_USB_SUFFIXES = {".gcode", ".bgcode", ".bff"}

    def __init__(self, identity: PrinterIdentity) -> None:
        self.identity = identity
        self.config = load_config()
        self.oauth = self.config["oauth"]
        self.mqtt_cfg = self.config["mqtt"]
        self.proxy_cfg = self.config["proxy"]["mqtt"]
        self.access_token = ""
        self.mqtt_username = ""
        self.client: mqtt.Client | None = None
        self.mqtt_connected = False
        self.running = True
        self.printing = False
        self.paused = False
        self.ready = False
        self.job_id = 1001
        self.current_path = "/usb/mock/demo_cube.bgcode"
        self.job_progress = 0
        self.time_remaining = 0
        self.axis_x = 125.0
        self.axis_y = 105.0
        self.axis_z = 5.0
        self._relative_positioning = False
        self.connect_token = self.identity.connect_token
        self.pending_transfer: TransferSession | None = None
        self.cancelable_objects: dict[int, bool] = {0: False, 1: False}
        self.settings: dict[str, Any] = {}
        self.next_dialog_id = 100
        self.telemetry_period_idle_s = 4.0
        self.telemetry_period_printing_s = 1.0
        self.last_telemetry_at = 0.0
        self.last_full_telemetry_at = 0.0
        default_usb_root = Path("/tmp/mk4mock") / self.identity.serial_number
        self.usb_root = Path(os.getenv("MOCK_USB_ROOT", str(default_usb_root))).resolve()
        self.usb_root.mkdir(parents=True, exist_ok=True)
        self._subscribe_topics: dict[int, str] = {}

    @property
    def event_topic(self) -> str:
        return self.proxy_cfg.get("event_topic", "v1/devices/printers/+/event").replace("+", self.mqtt_username)

    @property
    def command_topic(self) -> str:
        template = self.proxy_cfg.get("command_topic_template", "v1/devices/printers/{device_id}/cmd")
        return template.format(device_id=self.mqtt_username)

    @property
    def command_gcode_topic(self) -> str:
        return self.proxy_cfg.get("gcode_topic", "v1/devices/printers/+/gcode").replace("+", self.mqtt_username)

    @property
    def command_transfer_topic(self) -> str:
        return self.proxy_cfg.get("transfer_topic", "v1/devices/printers/+/transfer").replace("+", self.mqtt_username)

    @property
    def command_debug_topic(self) -> str:
        return self.proxy_cfg.get("debug_command_topic", "v1/devices/printers/+/debug").replace("+", self.mqtt_username)

    @property
    def online_topic(self) -> str:
        return self.proxy_cfg.get("online_topic", "v1/devices/printers/+/data/online").replace("+", self.mqtt_username)

    @property
    def telemetry_topic(self) -> str:
        return self.proxy_cfg.get("telemetry_topic", "v1/devices/printers/+/telemetry").replace("+", self.mqtt_username)

    @property
    def debug_topic(self) -> str:
        return self.proxy_cfg.get("debug_topic", "v1/devices/printers/+/debug").replace("+", self.mqtt_username)

    def _oauth_base_url(self) -> str:
        return os.getenv("OAUTH_BASE_URL", "https://127.0.0.1:8443").rstrip("/")

    def _oauth_ca_file(self) -> str:
        return os.getenv("OAUTH_CA_FILE", str(Path(__file__).resolve().parents[1] / "certs" / "ca.crt"))

    def _oauth_insecure(self) -> bool:
        return os.getenv("OAUTH_INSECURE", "1").lower() in {"1", "true", "yes"}

    def _resolve_host_path(self, configured_path: str) -> str:
        """Translate sandbox/container paths to host-local paths for dev scripts.

        `sandbox.yaml` is written primarily for docker-compose services, so TLS
        files typically point to `/certs/...`. When the mock runs on the host,
        those paths do not exist. In that case we transparently remap them to
        `utils/oauth-mqtt/certs/...`.
        """

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

    def obtain_access_token(self) -> None:
        """Use the sandbox device flow so the simulator authenticates like FW."""

        base_url = self._oauth_base_url()
        insecure = self._oauth_insecure()
        ca_file = self._oauth_ca_file()

        device = _request_json(
            f"{base_url}/oauth/device_authorization",
            {"client_id": "mock-printer", "scope": "mqtt"},
            ca_file,
            insecure,
        )
        _post_form(
            f"{base_url}/oauth/verify",
            {"user_code": device["user_code"]},
            ca_file,
            insecure,
        )
        tokens = _request_json(
            f"{base_url}/oauth/token",
            {
                "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                "device_code": device["device_code"],
                "client_id": "mock-printer",
                "sn": self.identity.serial_number,
            },
            ca_file,
            insecure,
        )
        self.access_token = str(tokens["access_token"])
        claims = _decode_jwt_payload(self.access_token)
        self.mqtt_username = str(claims.get("mqtt_username") or self.identity.device_id)
        LOGGER.info(
            "obtained MQTT credentials sn=%s mqtt_username=%s",
            self.identity.serial_number,
            self.mqtt_username,
        )

    def connect_mqtt(self) -> None:
        host = os.getenv("MOCK_MQTT_HOST", "127.0.0.1")
        port = int(os.getenv("MOCK_MQTT_PORT", "8883"))
        client_id = f"mock-printer-{self.mqtt_username}"
        self.client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)
        self.client.username_pw_set(self.mqtt_username, self.access_token)
        ca_file = self._resolve_host_path(str(self.mqtt_cfg["tls"]["ca_file"]))
        self.client.tls_set(
            ca_certs=ca_file,
            certfile=None,
            keyfile=None,
            tls_version=ssl.PROTOCOL_TLS_CLIENT,
            ciphers=self.oauth["tls"]["ciphers"],
        )
        if self.mqtt_cfg["tls"].get("insecure", False):
            self.client.tls_insecure_set(True)
        self.client.will_set(self.online_topic, payload="0", qos=1, retain=True)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.client.on_subscribe = self._on_subscribe
        LOGGER.info("connecting to MQTT host=%s port=%d cmd=%s event=%s", host, port, self.command_topic, self.event_topic)
        self.client.connect(host, port, keepalive=30)
        self.client.loop_start()

    def _publish_event(self, envelope: Any) -> None:
        if self.client is None:
            raise RuntimeError("MQTT client not initialized")
        if not self.mqtt_connected:
            return
        payload = envelope.SerializeToString()
        LOGGER.info("publishing event=%s len=%d", event_pb2.EventType.Name(envelope.event), len(payload))
        self.client.publish(self.event_topic, payload=payload, qos=1)

    def _publish_telemetry(self, force: bool = False, force_full: bool = False) -> None:
        if self.client is None:
            raise RuntimeError("MQTT client not initialized")
        if not self.mqtt_connected:
            return
        now = time.monotonic()
        period = self.telemetry_period_printing_s if self.printing else self.telemetry_period_idle_s
        if not force and (now - self.last_telemetry_at) < period:
            return
        send_full = force_full or (now - self.last_full_telemetry_at) >= 300.0

        telemetry = telemetry_pb2.TelemetryEnvelope()
        telemetry.state = self._state_name()
        if send_full:
            telemetry.has_axis_z = True
            telemetry.axis_z = self.axis_z if not self.printing else max(self.axis_z, 0.2)
            telemetry.has_temp_nozzle = True
            telemetry.temp_nozzle = 215.0 if self.printing else 30.0
            telemetry.has_temp_bed = True
            telemetry.temp_bed = 60.0 if self.printing else 28.0
            telemetry.has_target_nozzle = True
            telemetry.target_nozzle = 215.0 if self.printing else 0.0
            telemetry.has_target_bed = True
            telemetry.target_bed = 60.0 if self.printing else 0.0
            telemetry.has_speed = True
            telemetry.speed = 100
            telemetry.has_flow = True
            telemetry.flow = 100
            telemetry.has_material = True
            telemetry.material = "PLA"
        if send_full and not self.printing:
            telemetry.has_axis_x = True
            telemetry.axis_x = self.axis_x
            telemetry.has_axis_y = True
            telemetry.axis_y = self.axis_y
        if self.printing:
            telemetry.has_job_id = True
            telemetry.job_id = self.job_id
            telemetry.has_progress = True
            telemetry.progress = self.job_progress
            telemetry.has_time_printing = True
            telemetry.time_printing = min(self.job_progress * 12, 3600)
            telemetry.has_time_remaining = True
            telemetry.time_remaining = self.time_remaining
            if send_full:
                telemetry.has_fan_extruder = True
                telemetry.fan_extruder = 2200
                telemetry.has_fan_print = True
                telemetry.fan_print = 1800
                telemetry.has_filament = True
                telemetry.filament = round(self.job_progress * 0.6, 1)
            if not self.paused:
                self.job_progress = min(self.job_progress + 1, 100)
                self.time_remaining = max(self.time_remaining - 5, 0)
                if self.job_progress >= 100:
                    self.printing = False
                    self.paused = False

        if self.pending_transfer is not None:
            telemetry.has_transfer_id = True
            telemetry.transfer_id = self.pending_transfer.start_cmd_id
            telemetry.has_transfer_transferred = True
            telemetry.transfer_transferred = self.pending_transfer.transferred
            telemetry.has_transfer_time_remaining = True
            telemetry.transfer_time_remaining = self.pending_transfer.time_remaining
            telemetry.has_transfer_progress = True
            telemetry.transfer_progress = self.pending_transfer.progress

        payload = telemetry.SerializeToString()
        LOGGER.info("publishing telemetry state=%s full=%s len=%d", telemetry.state, send_full, len(payload))
        self.client.publish(self.telemetry_topic, payload=payload, qos=1)
        self.last_telemetry_at = now
        if send_full:
            self.last_full_telemetry_at = now

    def _resolve_usb_path(self, requested_path: str) -> Path:
        relative = requested_path.removeprefix("/usb").lstrip("/")
        resolved = (self.usb_root / relative).resolve()
        self._ensure_usb_scope(resolved)
        return resolved

    def _ensure_usb_scope(self, resolved_path: Path) -> None:
        if resolved_path == self.usb_root:
            return
        if self.usb_root not in resolved_path.parents:
            raise ValueError("Forbidden path")

    def _list_usb_children(self, requested_path: str) -> list[Path]:
        try:
            target = self._resolve_usb_path(requested_path)
        except ValueError:
            return []
        if not target.exists() or not target.is_dir():
            return []
        # Mirror printer UI behavior: hide dotfiles/folders and non-printable file types.
        def _is_visible(item: Path) -> bool:
            if item.name.startswith("."):
                return False
            if item.is_dir():
                return True
            return item.suffix.lower() in self.ALLOWED_USB_SUFFIXES

        return sorted(
            [item for item in target.iterdir() if _is_visible(item)],
            key=lambda item: item.name.lower(),
        )

    def _free_space(self) -> int:
        stat = os.statvfs(self.usb_root)
        return int(stat.f_frsize * stat.f_bavail)

    def _emit_file_changed(self, command_id: int, old_path: str, new_path: str) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_FILE_CHANGED
        env.state = self._state_name()
        env.has_command_id = command_id > 0
        env.command_id = command_id
        env.file_changed.free_space = self._free_space()
        env.file_changed.old_path = old_path
        env.file_changed.new_path = new_path
        env.file_changed.rescan = False
        target = new_path or old_path
        if target:
            try:
                path = self._resolve_usb_path(target)
                if path.exists():
                    entry = env.file_changed.file
                    entry.name = path.name.upper()
                    entry.display_name = path.name
                    entry.type = "FILE" if path.is_file() else "FOLDER"
                    entry.m_timestamp = int(path.stat().st_mtime)
                    entry.size = path.stat().st_size if path.is_file() else 0
            except Exception:
                pass
        self._publish_event(env)

    def _publish_finished(self, command_id: int) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_FINISHED
        env.state = self._state_name()
        env.has_command_id = True
        env.command_id = command_id
        env.finished.SetInParent()
        self._publish_event(env)

    def _publish_failed(self, command_id: int, reason: str) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_FAILED
        env.state = self._state_name()
        env.has_command_id = True
        env.command_id = command_id
        env.has_reason = True
        env.reason = reason
        env.failed.reason = reason
        self._publish_event(env)

    def _publish_transfer_info(self, command_id: int = 0, start_cmd_id: int = 0) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_TRANSFER_INFO
        env.state = self._state_name()
        env.has_command_id = command_id > 0
        env.command_id = command_id
        if self.pending_transfer is None:
            env.transfer_info.type = event_pb2.TRANSFER_TYPE_NO_TRANSFER
        else:
            transfer = self.pending_transfer
            env.transfer_info.type = transfer.transfer_type
            env.transfer_info.size = transfer.size
            env.transfer_info.transferred = transfer.transferred
            env.transfer_info.progress = transfer.progress
            env.transfer_info.time_transferring = transfer.time_transferring
            env.transfer_info.time_remaining = transfer.time_remaining
            env.transfer_info.path = transfer.path
            env.transfer_info.has_start_cmd_id = True
            env.transfer_info.start_cmd_id = start_cmd_id or transfer.start_cmd_id
        self._publish_event(env)

    def _publish_transfer_terminal(self, event_type: int, start_cmd_id: int, command_id: int = 0) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_type
        env.state = self._state_name()
        env.has_command_id = command_id > 0
        env.command_id = command_id
        if event_type == event_pb2.EVENT_TYPE_TRANSFER_STOPPED:
            env.transfer_stopped.has_start_cmd_id = True
            env.transfer_stopped.start_cmd_id = start_cmd_id
        elif event_type == event_pb2.EVENT_TYPE_TRANSFER_ABORTED:
            env.transfer_aborted.has_start_cmd_id = True
            env.transfer_aborted.start_cmd_id = start_cmd_id
        elif event_type == event_pb2.EVENT_TYPE_TRANSFER_FINISHED:
            env.transfer_finished.has_start_cmd_id = True
            env.transfer_finished.start_cmd_id = start_cmd_id
        self._publish_event(env)

    def _publish_cancelable_changed(self, command_id: int) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_CANCELABLE_CHANGED
        env.state = self._state_name()
        env.has_command_id = command_id > 0
        env.command_id = command_id
        for object_id, canceled in sorted(self.cancelable_objects.items()):
            item = env.cancelable_changed.objects.add()
            item.id = int(object_id)
            item.canceled = bool(canceled)
        self._publish_event(env)

    def _publish_info(self, command_id: int = 0) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_INFO
        env.state = self._state_name()
        env.has_command_id = command_id > 0
        env.command_id = command_id
        env.info.firmware = self.identity.firmware_version or "Buddy Mock 0.1"
        env.info.printer_type = self.identity.printer_model_id or "MK4"
        env.info.sn = self.identity.serial_number
        env.info.appendix = False
        env.info.fingerprint = self.identity.connect_fingerprint or "mock-fingerprint"
        env.info.nozzle_diameter = 0.4
        env.info.transfer_paused = False
        env.info.api_key = ""
        storage = env.info.storages.add()
        storage.mountpoint = "/usb"
        storage.type = "USB"
        storage.read_only = False
        storage.free_space = 512 * 1024 * 1024
        storage.is_sfn = False
        env.info.network_info.hostname = "buddy-mock"
        env.info.network_info.lan_ipv4 = "192.168.1.50"
        tool = env.info.tools.add()
        tool.slot = 0
        tool.nozzle_diameter = 0.4
        tool.material = "PLA"
        env.info.mmu.enabled = False
        env.info.addon_power = False
        env.info.slots = 1
        self._publish_event(env)

    def _publish_file_info(self, path: str, command_id: int) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_FILE_INFO
        env.state = self._state_name()
        env.has_command_id = True
        env.command_id = command_id
        requested_path = path or "/usb"
        target = self._resolve_usb_path(requested_path)
        env.file_info.path = requested_path
        env.file_info.display_name = target.name if requested_path != "/usb" else "usb"
        env.file_info.read_only = False
        if target.exists() and target.is_file():
            stat = target.stat()
            env.file_info.type = "FILE"
            env.file_info.size = stat.st_size
            env.file_info.m_timestamp = int(stat.st_mtime)
            env.file_info.file_count = 0
        else:
            env.file_info.type = "FOLDER"
            children = self._list_usb_children(requested_path)
            env.file_info.file_count = len(children)
            for child in children[:32]:
                stat = child.stat()
                entry = env.file_info.children.add()
                entry.name = child.name.upper()
                entry.display_name = child.name
                entry.size = stat.st_size if child.is_file() else 0
                entry.m_timestamp = int(stat.st_mtime)
                entry.type = "FILE" if child.is_file() else "FOLDER"
        self._publish_event(env)

    def _publish_job_info(self, command_id: int) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_JOB_INFO
        env.state = self._state_name()
        env.has_command_id = True
        env.command_id = command_id
        env.job_info.has_job_id = True
        env.job_info.job_id = self.job_id
        env.job_info.state = self._state_name()
        env.job_info.size = 1234567
        env.job_info.m_timestamp = int(time.time())
        env.job_info.display_name = "demo_cube.bgcode"
        env.job_info.start_cmd_id = command_id
        env.job_info.path = self.current_path
        self._publish_event(env)

    def _publish_state_changed(self, command_id: int = 0) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_STATE_CHANGED
        env.state = self._state_name()
        env.has_command_id = command_id > 0
        env.command_id = command_id
        env.state_changed.has_title = True
        env.state_changed.title = f"Mock state: {self._state_name()}"
        env.state_changed.has_text = True
        env.state_changed.text = "Generated by mock_printer.py"
        self._publish_event(env)

    def _publish_accepted(self, command_id: int) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_ACCEPTED
        env.state = self._state_name()
        env.has_command_id = True
        env.command_id = command_id
        env.accepted.SetInParent()
        self._publish_event(env)

    def _publish_rejected(self, command_id: int, reason: str, machine_reason: int = event_pb2.MACHINE_REASON_UNSPECIFIED) -> None:
        env = event_pb2.EventEnvelope()
        env.event = event_pb2.EVENT_TYPE_REJECTED
        env.state = self._state_name()
        env.has_command_id = True
        env.command_id = command_id
        env.has_reason = True
        env.reason = reason
        env.machine_reason = machine_reason
        env.rejected.reason = reason
        self._publish_event(env)

    def _publish_debug(self, message: str, level: int = debug_pb2.DEBUG_LEVEL_INFO, command_id: int = 0) -> None:
        if self.client is None:
            return
        env = debug_pb2.DebugMessageEnvelope()
        env.command_id = int(command_id)
        env.level = level
        env.source = "mock_printer"
        env.message = message
        env.timestamp_ms = int(time.time() * 1000)
        self.client.publish(self.debug_topic, payload=env.SerializeToString(), qos=1)

    def _execute_gcode(self, gcode: str, command_id: int, force: bool) -> None:
        lines = [line.strip() for line in gcode.replace("\r", "").split("\n") if line.strip()]
        if not lines:
            self._publish_rejected(command_id, "Empty gcode")
            return
        for line in lines:
            line = line.split(";", 1)[0].strip()
            if not line:
                continue
            upper = line.upper()
            if upper.startswith("G90"):
                self._relative_positioning = False
                continue
            if upper.startswith("G91"):
                self._relative_positioning = True
                continue
            if upper.startswith("G28"):
                axis_part = upper[3:].strip()
                home_x = (not axis_part) or ("X" in axis_part)
                home_y = (not axis_part) or ("Y" in axis_part)
                home_z = (not axis_part) or ("Z" in axis_part)
                if home_x:
                    self.axis_x = 0.0
                if home_y:
                    self.axis_y = 0.0
                if home_z:
                    self.axis_z = 0.0
                continue
            if upper.startswith("G0") or upper.startswith("G1"):
                values: dict[str, float] = {}
                for token in upper.split()[1:]:
                    if len(token) < 2:
                        continue
                    axis = token[0]
                    if axis not in {"X", "Y", "Z"}:
                        continue
                    try:
                        values[axis] = float(token[1:])
                    except ValueError:
                        continue
                for axis, value in values.items():
                    if axis == "X":
                        self.axis_x = self.axis_x + value if self._relative_positioning else value
                    elif axis == "Y":
                        self.axis_y = self.axis_y + value if self._relative_positioning else value
                    elif axis == "Z":
                        self.axis_z = self.axis_z + value if self._relative_positioning else value
                continue
            LOGGER.info("mock gcode ignored line=%s force=%s", line, force)
        self._publish_finished(command_id)
        self._publish_telemetry(force=True, force_full=True)

    def _state_name(self) -> str:
        if self.printing and self.paused:
            return "PAUSED"
        if self.printing:
            return "PRINTING"
        if self.ready:
            return "READY"
        return "IDLE"

    def _handle_command(self, envelope: Any) -> None:
        which = envelope.WhichOneof("payload")
        command_id = int(envelope.command_id)
        LOGGER.info("received command=%s command_id=%d", which, command_id)

        if which == "send_info":
            self._publish_info(command_id)
        elif which == "send_file_info":
            path = envelope.send_file_info.path or "/usb"
            try:
                target = self._resolve_usb_path(path)
            except ValueError:
                self._publish_rejected(command_id, "Forbidden path")
                return
            if not target.exists():
                self._publish_rejected(command_id, "File not found")
                return
            if target.is_file() and target.suffix.lower() not in self.ALLOWED_USB_SUFFIXES:
                self._publish_rejected(command_id, "Unsupported file type")
                return
            self._publish_file_info(path, command_id)
        elif which == "send_job_info":
            request = envelope.send_job_info
            if not self.printing:
                self._publish_rejected(command_id, "No active job")
                return
            if request.has_job_id and int(request.job_id) != int(self.job_id):
                self._publish_rejected(command_id, "Job id mismatch")
                return
            self._publish_job_info(command_id)
        elif which == "start_print":
            requested = envelope.start_print.path or self.current_path
            try:
                target = self._resolve_usb_path(requested)
            except ValueError:
                self._publish_rejected(command_id, "Forbidden path")
                return
            if not target.exists() or not target.is_file():
                self._publish_rejected(command_id, "File not found")
                return
            if target.suffix.lower() not in self.ALLOWED_USB_SUFFIXES:
                self._publish_rejected(command_id, "Unsupported file type")
                return
            self.current_path = requested
            self.printing = True
            self.paused = False
            self.ready = False
            self.job_progress = 0
            self.time_remaining = 600
            self._publish_job_info(command_id)
            self._publish_telemetry(force=True, force_full=True)
        elif which == "pause_print":
            if not self.printing:
                self._publish_rejected(command_id, "No active print")
                return
            self.paused = True
            self._publish_finished(command_id)
            self._publish_state_changed(command_id)
            self._publish_telemetry(force=True, force_full=True)
        elif which == "resume_print":
            if not self.printing:
                self._publish_rejected(command_id, "No active print")
                return
            self.paused = False
            self._publish_finished(command_id)
            self._publish_state_changed(command_id)
            self._publish_telemetry(force=True, force_full=True)
        elif which == "stop_print":
            if not self.printing:
                self._publish_rejected(command_id, "No active print")
                return
            self.printing = False
            self.paused = False
            self.ready = False
            self.job_progress = 0
            self.time_remaining = 0
            self._publish_finished(command_id)
            self._publish_state_changed(command_id)
            self._publish_telemetry(force=True, force_full=True)
        elif which == "reset_printer":
            self.printing = False
            self.paused = False
            self.ready = False
            self.job_progress = 0
            self.time_remaining = 0
            self.pending_transfer = None
            self._publish_finished(command_id)
            self._publish_state_changed(command_id)
            self._publish_info(command_id)
            self._publish_telemetry(force=True, force_full=True)
        elif which == "send_state_info":
            self._publish_state_changed(command_id)
        elif which == "send_transfer_info":
            self._publish_transfer_info(command_id)
        elif which == "set_printer_ready":
            if self.printing:
                self._publish_rejected(command_id, "Can't set ready now")
                return
            self.ready = True
            self._publish_state_changed(command_id)
            self._publish_telemetry(force=True)
        elif which == "cancel_printer_ready":
            self.ready = False
            self._publish_finished(command_id)
            self._publish_state_changed(command_id)
        elif which == "set_idle":
            if self.printing:
                self._publish_rejected(command_id, "Can't set idle now")
                return
            self.ready = False
            self._publish_finished(command_id)
            self._publish_state_changed(command_id)
            self._publish_telemetry(force=True)
        elif which == "delete_file":
            path = envelope.delete_file.path
            try:
                target = self._resolve_usb_path(path)
            except ValueError:
                self._publish_rejected(command_id, "Forbidden path")
                return
            if not target.exists() or not target.is_file():
                self._publish_rejected(command_id, "File not found")
                return
            target.unlink(missing_ok=False)
            self._emit_file_changed(command_id, old_path=path, new_path="")
        elif which == "delete_folder":
            path = envelope.delete_folder.path
            try:
                target = self._resolve_usb_path(path)
            except ValueError:
                self._publish_rejected(command_id, "Forbidden path")
                return
            if not target.exists() or not target.is_dir():
                self._publish_rejected(command_id, "File not found")
                return
            try:
                target.rmdir()
            except OSError:
                self._publish_rejected(command_id, "Directory not empty")
                return
            self._emit_file_changed(command_id, old_path=path, new_path="")
        elif which == "create_folder":
            path = envelope.create_folder.path
            try:
                target = self._resolve_usb_path(path)
            except ValueError:
                self._publish_rejected(command_id, "Forbidden path")
                return
            try:
                target.mkdir(parents=False, exist_ok=False)
            except FileExistsError:
                self._publish_rejected(command_id, "Directory already exists")
                return
            except OSError:
                self._publish_rejected(command_id, "Error creating directory")
                return
            self._emit_file_changed(command_id, old_path="", new_path=path)
        elif which == "stop_transfer":
            if self.pending_transfer is None:
                self._publish_rejected(command_id, "No transfer in progress")
                return
            started = self.pending_transfer.start_cmd_id
            self.pending_transfer.stopped = True
            self.pending_transfer = None
            self._publish_finished(command_id)
            self._publish_transfer_terminal(event_pb2.EVENT_TYPE_TRANSFER_STOPPED, start_cmd_id=started, command_id=command_id)
        elif which == "set_token":
            self.connect_token = envelope.set_token.token
            self._publish_finished(command_id)
        elif which == "dialog_action":
            if envelope.dialog_action.dialog_id != self.next_dialog_id:
                self._publish_rejected(command_id, "Unknown dialog")
                return
            self.next_dialog_id += 1
            self._publish_finished(command_id)
        elif which == "set_value":
            value_field = envelope.set_value.WhichOneof("value")
            value: Any = None
            if value_field is not None:
                value = getattr(envelope.set_value, value_field)
            self.settings[f"property_{int(envelope.set_value.property)}"] = {
                "tool_index": int(envelope.set_value.tool_index),
                "value": value,
            }
            self._publish_finished(command_id)
        elif which == "cancel_object":
            self.cancelable_objects[int(envelope.cancel_object.object_id)] = True
            self._publish_cancelable_changed(command_id)
        elif which == "uncancel_object":
            self.cancelable_objects[int(envelope.uncancel_object.object_id)] = False
            self._publish_cancelable_changed(command_id)
        elif which in {"start_inline_download", "start_connect_download", "start_encrypted_download"}:
            if self.pending_transfer is not None:
                self._publish_rejected(
                    command_id,
                    "Another transfer in progress",
                    machine_reason=event_pb2.MACHINE_REASON_TRANSFER_IN_PROGRESS,
                )
                return
            if which == "start_inline_download":
                download = envelope.start_inline_download
                transfer_type = event_pb2.TRANSFER_TYPE_LINK
            elif which == "start_connect_download":
                download = envelope.start_connect_download
                transfer_type = event_pb2.TRANSFER_TYPE_CONNECT
            else:
                download = envelope.start_encrypted_download
                transfer_type = event_pb2.TRANSFER_TYPE_CONNECT
                if len(download.key) != 16 or len(download.iv) != 16:
                    self._publish_rejected(command_id, "Invalid key/iv length")
                    return
            try:
                target = self._resolve_usb_path(download.path)
            except ValueError:
                self._publish_rejected(command_id, "Forbidden path")
                return
            if int(download.orig_size) <= 0:
                self._publish_rejected(command_id, "Invalid file size")
                return
            if target.exists():
                self._publish_rejected(command_id, "File already exists", machine_reason=event_pb2.MACHINE_REASON_FILE_EXISTS)
                return
            self.pending_transfer = TransferSession(
                start_cmd_id=command_id,
                path=download.path,
                transfer_type=transfer_type,
                size=int(download.orig_size),
                wait_for_chunks=(which == "start_inline_download"),
                started_at=time.time(),
            )
            self._publish_transfer_info(command_id, start_cmd_id=command_id)
        else:
            self._publish_rejected(command_id, f"Unsupported mock command: {which}")

    def _handle_gcode_command(self, envelope: Any) -> None:
        command_id = int(envelope.command_id)
        gcode = str(envelope.gcode)
        force = bool(envelope.force)
        LOGGER.info("received gcode command command_id=%d force=%s len=%d", command_id, force, len(gcode))
        self._execute_gcode(gcode, command_id=command_id, force=force)

    def _handle_transfer_chunk(self, envelope: Any) -> None:
        transfer = self.pending_transfer
        if transfer is None:
            self._publish_debug(
                f"transfer chunk ignored: no pending transfer (transfer_id={int(envelope.transfer_id)})",
                level=debug_pb2.DEBUG_LEVEL_WARN,
                command_id=int(envelope.command_id),
            )
            return
        if int(envelope.transfer_id) != int(transfer.start_cmd_id):
            self._publish_debug(
                f"transfer chunk ignored: transfer_id mismatch got={int(envelope.transfer_id)} expected={transfer.start_cmd_id}",
                level=debug_pb2.DEBUG_LEVEL_WARN,
                command_id=int(envelope.command_id),
            )
            return
        transfer.expects_chunks = True
        payload = bytes(envelope.data)
        if bool(envelope.has_crc32):
            checksum = zlib.crc32(payload) & 0xFFFFFFFF
            if checksum != int(envelope.crc32):
                self._publish_failed(int(envelope.command_id), "Chunk CRC mismatch")
                self._publish_transfer_terminal(
                    event_pb2.EVENT_TYPE_TRANSFER_ABORTED,
                    start_cmd_id=transfer.start_cmd_id,
                    command_id=int(envelope.command_id),
                )
                self.pending_transfer = None
                return
        try:
            target = self._resolve_usb_path(transfer.path)
            target.parent.mkdir(parents=True, exist_ok=True)
            mode = "wb" if int(envelope.chunk_index) == 0 else "ab"
            with target.open(mode) as handle:
                handle.write(payload)
        except Exception as exc:
            self._publish_failed(int(envelope.command_id), f"Storage failure: {exc}")
            self._publish_transfer_terminal(
                event_pb2.EVENT_TYPE_TRANSFER_ABORTED,
                start_cmd_id=transfer.start_cmd_id,
                command_id=int(envelope.command_id),
            )
            self.pending_transfer = None
            return
        transfer.transferred = min(transfer.size, transfer.transferred + len(payload))
        transfer.time_transferring += 1
        self._publish_transfer_info(command_id=int(envelope.command_id), start_cmd_id=transfer.start_cmd_id)
        if bool(envelope.last) or transfer.transferred >= transfer.size:
            self._publish_transfer_terminal(
                event_pb2.EVENT_TYPE_TRANSFER_FINISHED,
                start_cmd_id=transfer.start_cmd_id,
                command_id=int(envelope.command_id),
            )
            self._emit_file_changed(command_id=0, old_path="", new_path=transfer.path)
            self.pending_transfer = None

    def _handle_debug_command(self, envelope: Any) -> None:
        LOGGER.info(
            "received debug command command_id=%d level=%s source=%s message=%s",
            int(envelope.command_id),
            int(envelope.level),
            envelope.source,
            envelope.message,
        )
        self._publish_debug(
            f"debug command received source={envelope.source}: {envelope.message}",
            level=debug_pb2.DEBUG_LEVEL_INFO,
            command_id=int(envelope.command_id),
        )

    def _tick_transfer(self) -> None:
        if self.pending_transfer is None:
            return
        transfer = self.pending_transfer
        if transfer.wait_for_chunks and not transfer.expects_chunks:
            # Give chunk mode a short grace period before fallback auto-complete.
            if (time.time() - transfer.started_at) < 2.0:
                return
            transfer.wait_for_chunks = False
        if transfer.expects_chunks:
            return
        transfer.time_transferring += 1
        chunk = max(32 * 1024, transfer.size // 20)
        transfer.transferred = min(transfer.size, transfer.transferred + chunk)
        if transfer.transferred >= transfer.size:
            try:
                target = self._resolve_usb_path(transfer.path)
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.exists():
                    target.write_bytes(b"; mock downloaded file\n")
            except Exception as exc:
                self._publish_failed(transfer.start_cmd_id, f"Storage failure: {exc}")
                self._publish_transfer_terminal(event_pb2.EVENT_TYPE_TRANSFER_ABORTED, start_cmd_id=transfer.start_cmd_id)
                self.pending_transfer = None
                return
            self._publish_transfer_terminal(event_pb2.EVENT_TYPE_TRANSFER_FINISHED, start_cmd_id=transfer.start_cmd_id)
            self._emit_file_changed(command_id=0, old_path="", new_path=transfer.path)
            self.pending_transfer = None

    def _on_connect(self, client: mqtt.Client, _userdata: object, _flags: dict[str, int], rc: int) -> None:
        if rc != 0:
            raise RuntimeError(f"MQTT connect failed rc={rc}")
        self.mqtt_connected = True
        topics = [
            self.command_topic,
            self.command_gcode_topic,
            self.command_transfer_topic,
            self.command_debug_topic,
        ]
        LOGGER.info("MQTT connected, subscribing to %s", topics)
        for topic in topics:
            sub_result, sub_mid = client.subscribe(topic, qos=1)
            self._subscribe_topics[sub_mid] = topic
            LOGGER.info("subscribe request result=%s mid=%s topic=%s", sub_result, sub_mid, topic)
        client.publish(self.online_topic, payload="1", qos=1, retain=True)
        self._publish_info()
        self._publish_state_changed()
        self._publish_telemetry(force=True, force_full=True)

    def _on_message(self, _client: mqtt.Client, _userdata: object, msg: mqtt.MQTTMessage) -> None:
        topic = msg.topic if isinstance(msg.topic, str) else msg.topic.decode("utf-8", errors="replace")
        payload = bytes(msg.payload)
        LOGGER.info("received MQTT cmd topic=%s len=%d", topic, len(payload))
        try:
            if topic == self.command_topic:
                envelope = command_pb2.CommandEnvelope()
                envelope.ParseFromString(payload)
                self._handle_command(envelope)
                return
            if topic == self.command_gcode_topic:
                envelope = gcode_pb2.GcodeCommandEnvelope()
                envelope.ParseFromString(payload)
                self._handle_gcode_command(envelope)
                return
            if topic == self.command_transfer_topic:
                envelope = transfer_pb2.TransferChunkEnvelope()
                envelope.ParseFromString(payload)
                self._handle_transfer_chunk(envelope)
                return
            if topic == self.command_debug_topic:
                envelope = debug_pb2.DebugMessageEnvelope()
                envelope.ParseFromString(payload)
                self._handle_debug_command(envelope)
                return
            LOGGER.warning("unsupported command topic=%s", topic)
        except Exception:
            LOGGER.exception("failed to process command payload on topic=%s", topic)

    def _on_disconnect(self, _client: mqtt.Client, _userdata: object, rc: int) -> None:
        self.mqtt_connected = False
        LOGGER.warning("MQTT disconnected rc=%s", rc)

    def _on_subscribe(
        self,
        _client: mqtt.Client,
        _userdata: object,
        mid: int,
        granted_qos: tuple[int, ...],
        _properties: Any = None,
    ) -> None:
        topic = self._subscribe_topics.pop(mid, "<unknown>")
        LOGGER.info("subscribe ack mid=%s granted_qos=%s topic=%s", mid, list(granted_qos), topic)

    def shutdown(self) -> None:
        self.running = False
        if self.client is not None:
            self.mqtt_connected = False
            self.client.publish(self.online_topic, payload="0", qos=1, retain=True)
            self.client.disconnect()
            self.client.loop_stop()

    def run(self) -> None:
        self.obtain_access_token()
        self.connect_mqtt()
        while self.running:
            self._tick_transfer()
            if self.pending_transfer is not None:
                self._publish_transfer_info()
            self._publish_telemetry()
            time.sleep(1)


def _resolve_printer(serial_number: str | None) -> PrinterIdentity:
    config = load_config()
    printers = configured_printers(config)
    if not printers:
        raise SystemExit("sandbox.yaml contains no printers")
    if serial_number:
        for printer in printers:
            if printer["serial_number"] == serial_number:
                return PrinterIdentity(**printer)
        raise SystemExit(f"printer with serial_number={serial_number!r} not found in sandbox.yaml")
    return PrinterIdentity(**printers[0])


def main() -> int:
    logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"))
    parser = ArgumentParser()
    parser.add_argument("serial_number", nargs="?")
    parser.add_argument("--usb-root", dest="usb_root", default=None)
    args = parser.parse_args()
    serial_number = args.serial_number
    printer = _resolve_printer(serial_number)
    app = MockPrinter(printer)
    if args.usb_root:
        app.usb_root = Path(args.usb_root).resolve()
        app.usb_root.mkdir(parents=True, exist_ok=True)
    LOGGER.info("using usb_root=%s", app.usb_root)

    def _stop(_signum: int, _frame: Any) -> None:
        LOGGER.info("received shutdown signal")
        app.shutdown()

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
