#!/usr/bin/env python3
from __future__ import annotations

import argparse
import curses
import hashlib
import json
import logging
import os
import shlex
import signal
import socket
import ssl
import sys
import threading
import time
import zlib
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from textwrap import wrap
from typing import Any

import paho.mqtt.client as mqtt

from proto_codegen import ensure_generated, output_dir
from connect_proxy.json_mapping import event_to_legacy_json
from connect_proxy.telemetry_json import telemetry_to_legacy_json
from connect_proxy.proto_codec import (
    build_command_from_json,
    command_pb2,
    decode_event,
    decode_telemetry,
    topic_device_id,
)
from oauth_server.mqtt_provisioning import DynSecProvisioner


ensure_generated()
generated_dir = output_dir()
if str(generated_dir) not in sys.path:
    sys.path.insert(0, str(generated_dir))

import event_pb2  # type: ignore  # noqa: E402
import debug_pb2  # type: ignore  # noqa: E402
import gcode_pb2  # type: ignore  # noqa: E402
import transfer_pb2  # type: ignore  # noqa: E402


LOGGER = logging.getLogger("mock_server")


@dataclass(slots=True)
class PrinterState:
    device_id: str
    online: bool = False
    last_seen: float = 0.0
    last_event_type: str = "-"
    last_event: dict[str, Any] = field(default_factory=dict)
    last_telemetry_delta: dict[str, Any] = field(default_factory=dict)
    last_telemetry: dict[str, Any] = field(default_factory=dict)
    command_seq: int = 1000
    pending_commands: dict[int, dict[str, Any]] = field(default_factory=dict)
    files: dict[str, dict[str, Any]] = field(default_factory=dict)
    job: dict[str, Any] = field(default_factory=dict)
    transfer: dict[str, Any] = field(default_factory=dict)
    sync_queue: deque[dict[str, Any]] = field(default_factory=deque)
    sync_inflight: bool = False
    sync_sent: int = 0
    sync_done: int = 0
    sync_failed: int = 0


class MockServerApp:
    TRANSFER_COMMANDS = {"START_INLINE_DOWNLOAD", "START_CONNECT_DOWNLOAD", "START_ENCRYPTED_DOWNLOAD"}

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.storage_root = args.storage_root.resolve()
        self.storage_root.mkdir(parents=True, exist_ok=True)

        self.running = True
        # MQTT callbacks and log rendering can nest lock usage, use reentrant lock
        # to avoid deadlocks when a callback logs while already holding state lock.
        self.lock = threading.RLock()
        self.printers: dict[str, PrinterState] = {}
        self.selected_device_id: str | None = None
        self.logs: deque[tuple[int, str]] = deque(maxlen=1000)
        self._next_log_id = 1
        self._last_printed_log_id = 0
        self.connected_event = threading.Event()
        self.connect_error: str | None = None
        self.mqtt_connected = False
        self.pending_timeout_s = 15

        self.client = mqtt.Client(client_id=args.client_id, protocol=mqtt.MQTTv311)
        self.client.username_pw_set(args.username, args.password)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        if args.ca_file:
            self.client.tls_set(
                ca_certs=str(args.ca_file),
                certfile=None,
                keyfile=None,
                tls_version=ssl.PROTOCOL_TLS_CLIENT,
            )
            if args.insecure:
                self.client.tls_insecure_set(True)
        # Avoid hanging forever during TCP/TLS connect.
        socket.setdefaulttimeout(args.connect_timeout)

    def _ensure_mock_server_acls(self) -> None:
        mqtt_cfg = {
            "enabled": True,
            "host": self.args.host,
            "port": self.args.port,
            "username": self.args.username,
            "password": self.args.password,
            "client_id": f"{self.args.client_id}-dynsec",
            "keepalive": self.args.keepalive,
            "tls": {
                "ca_file": str(self.args.ca_file) if self.args.ca_file else "",
                "insecure": bool(self.args.insecure),
                "cert_file": "",
                "key_file": "",
            },
            "control_topic": "$CONTROL/dynamic-security/v1",
            "response_topic": "$CONTROL/dynamic-security/v1/response",
        }
        role_name = "mock-server-role"
        role_acls = [
            {"acltype": "publishClientSend", "topic": "v1/devices/printers/#", "allow": True},
            {"acltype": "subscribePattern", "topic": "v1/devices/printers/#", "allow": True},
            {"acltype": "publishClientReceive", "topic": "v1/devices/printers/#", "allow": True},
        ]
        try:
            provisioner = DynSecProvisioner(mqtt_cfg)
            provisioner.connect()
            provisioner.ensure_role(role_name, role_acls)
            # DynSecProvisioner doesn't expose addClientRole helper publicly,
            # so we use its command API directly.
            responses = provisioner._send_commands(
                [
                    {
                        "command": "addClientRole",
                        "username": self.args.username,
                        "rolename": role_name,
                        "priority": 1,
                    }
                ]
            )
            provisioner._ignore_errors(responses, ["Client already has role", "Client not found"])
            provisioner.close()
            self._log(f"[SYS] ensured dynsec ACL role={role_name} for user={self.args.username}")
        except Exception as exc:
            self._log(f"[SYS] dynsec ACL bootstrap skipped: {exc}")

    def _printer(self, device_id: str) -> PrinterState:
        state = self.printers.get(device_id)
        if state is not None:
            return state
        state = PrinterState(device_id=device_id)
        self.printers[device_id] = state
        if self.selected_device_id is None:
            self.selected_device_id = device_id
        return state

    def _log(self, line: str) -> None:
        ts = time.strftime("%H:%M:%S")
        with self.lock:
            self.logs.append((self._next_log_id, f"{ts} {line}"))
            self._next_log_id += 1

    @staticmethod
    def _short_id(device_id: str) -> str:
        if len(device_id) <= 16:
            return device_id
        return f"{device_id[:8]}...{device_id[-6:]}"

    def _on_connect(self, client: mqtt.Client, _userdata: object, _flags: dict[str, int], rc: int) -> None:
        if rc != 0:
            self._log(f"[MQTT] connect failed rc={rc}")
            self.connect_error = f"rc={rc}"
            self.connected_event.set()
            return
        self.mqtt_connected = True
        subs = [
            "v1/devices/printers/+/data/online",
            "v1/devices/printers/+/data/#",
            "v1/devices/printers/+/jobs/+/data/#",
            "v1/devices/printers/+/dialog",
            "v1/devices/printers/+/event",
            "v1/devices/printers/+/telemetry",
            "v1/devices/printers/+/cmd",
            "v1/devices/printers/+/gcode",
            "v1/devices/printers/+/transfer",
            "v1/devices/printers/+/debug",
        ]
        for topic in subs:
            client.subscribe(topic, qos=1)
        self._log(f"[MQTT] connected host={self.args.host}:{self.args.port} subscribed={subs}")
        self.connected_event.set()

    def _on_disconnect(self, _client: mqtt.Client, _userdata: object, rc: int) -> None:
        self.mqtt_connected = False
        self._log(f"[MQTT] disconnected rc={rc}")
        if not self.connected_event.is_set():
            self.connect_error = f"disconnect rc={rc}"
            self.connected_event.set()

    @staticmethod
    def _parse_ascii_value(raw: str) -> Any:
        text = raw.strip()
        if not text:
            return text
        try:
            if any(ch in text for ch in (".", "e", "E")):
                return float(text)
            return int(text)
        except ValueError:
            return text

    def _decode_ascii_data_topic(self, topic: str, payload: bytes) -> dict[str, Any]:
        text = payload.decode("utf-8", errors="replace").strip()
        if not text:
            return {}

        # Legacy printer-level topics:
        # v1/devices/printers/<id>/data/<suffix>
        if "/data/" in topic and "/jobs/" not in topic:
            suffix = topic.split("/data/", 1)[1]
            mapping = {
                "state": "state",
                "axis-x": "axis_x",
                "axis-y": "axis_y",
                "axis-z": "axis_z",
                "current-job": "job_id",
                "job-progress": "progress",
                "temp/nozzle/current": "temp_nozzle",
                "temp/nozzle/target": "target_nozzle",
                "temp/heatbed/current": "temp_bed",
                "temp/heatbed/target": "target_bed",
                "speed": "speed",
                "flow": "flow",
                "material": "material",
            }
            key = mapping.get(suffix)
            if key is None:
                return {}
            value = text if key in {"state", "material"} else self._parse_ascii_value(text)
            return {key: value}

        # Legacy job-scoped topics:
        # v1/devices/printers/<id>/jobs/<job_id>/data/<suffix>
        if "/jobs/" in topic and "/data/" in topic:
            parts = topic.split("/")
            try:
                jobs_idx = parts.index("jobs")
                job_id = int(parts[jobs_idx + 1])
                if parts[jobs_idx + 2] != "data":
                    return {}
                suffix = "/".join(parts[jobs_idx + 3 :])
            except (ValueError, IndexError):
                return {}

            out: dict[str, Any] = {"job_id": job_id}
            if suffix == "state":
                out["state"] = text
            elif suffix == "progress":
                out["progress"] = self._parse_ascii_value(text)
            elif suffix == "time-remaining":
                out["time_remaining"] = self._parse_ascii_value(text)
            elif suffix == "time-printing":
                out["time_printing"] = self._parse_ascii_value(text)
            else:
                return {}
            return out

        # Dialog id published as plain number:
        # v1/devices/printers/<id>/dialog
        if topic.endswith("/dialog"):
            return {"dialog_id": self._parse_ascii_value(text)}

        return {}

    def _mark_command_response(self, state: PrinterState, payload: dict[str, Any]) -> dict[str, Any] | None:
        cmd_id = payload.get("command_id")
        if not isinstance(cmd_id, int):
            return None
        pending = state.pending_commands.pop(cmd_id, None)
        if pending is None:
            return None
        event_name = payload.get("event", "UNKNOWN")
        self._log(
            f"[ACK:{self._short_id(state.device_id)}] cmd_id={cmd_id} command={pending.get('command')} event={event_name}"
        )
        return pending

    def _persist_file_index(self, state: PrinterState) -> None:
        printer_dir = self.storage_root / state.device_id
        printer_dir.mkdir(parents=True, exist_ok=True)
        index_file = printer_dir / "files-index.json"
        index_payload = {
            "device_id": state.device_id,
            "updated_at": int(time.time()),
            "files": state.files,
        }
        index_file.write_text(json.dumps(index_payload, indent=2, sort_keys=True), encoding="utf-8")

    def _apply_file_event(self, state: PrinterState, payload: dict[str, Any]) -> None:
        event_name = payload.get("event")
        data = payload.get("data")
        if not isinstance(data, dict):
            return
        if event_name == "FILE_INFO":
            path = data.get("path")
            if isinstance(path, str):
                # Replace cached children under this folder when we get fresh listing.
                folder_prefix = path.rstrip("/") + "/"
                stale = [p for p in state.files.keys() if p.startswith(folder_prefix)]
                for stale_path in stale:
                    state.files.pop(stale_path, None)
                state.files[path] = data
                children = data.get("children")
                if isinstance(children, list):
                    for child in children:
                        if not isinstance(child, dict):
                            continue
                        child_name = child.get("display_name") or child.get("name")
                        if not isinstance(child_name, str) or not child_name:
                            continue
                        child_path = f"{path.rstrip('/')}/{child_name}"
                        child_data = dict(child)
                        child_data["path"] = child_path
                        state.files[child_path] = child_data
                self._persist_file_index(state)
        elif event_name == "FILE_CHANGED":
            new_path = data.get("new_path")
            old_path = data.get("old_path")
            if isinstance(old_path, str):
                state.files.pop(old_path, None)
            if isinstance(new_path, str):
                state.files[new_path] = data
            self._persist_file_index(state)

    def _apply_state_projection(self, state: PrinterState, payload: dict[str, Any]) -> None:
        event_name = payload.get("event")
        data = payload.get("data") if isinstance(payload.get("data"), dict) else {}
        if event_name == "JOB_INFO":
            state.job = dict(data)
            if "job_id" in payload:
                state.job["job_id"] = payload["job_id"]
        elif event_name == "TRANSFER_INFO":
            state.transfer = dict(data)
        elif event_name in {"TRANSFER_FINISHED", "TRANSFER_ABORTED", "TRANSFER_STOPPED"}:
            state.transfer = {"state": event_name}

    def _is_hidden_or_ignored_path(self, path: Path, root: Path) -> bool:
        try:
            rel = path.relative_to(root)
        except ValueError:
            return True
        for part in rel.parts:
            if part.startswith("."):
                return True
            if part == "__pycache__":
                return True
        return False

    def _maybe_dispatch_sync(self, device_id: str) -> None:
        with self.lock:
            state = self.printers.get(device_id)
            if state is None or state.sync_inflight or not state.sync_queue:
                return
            queued = state.sync_queue.popleft()
            command_name = str(queued.get("command", "START_INLINE_DOWNLOAD"))
            kwargs = dict(queued.get("kwargs") or {})
            state.sync_inflight = True
        cmd_id = self._publish_command(device_id, command_name, kwargs)
        if cmd_id is None:
            with self.lock:
                state = self.printers.get(device_id)
                if state is not None:
                    state.sync_inflight = False
                    state.sync_failed += 1
            return
        with self.lock:
            state = self.printers.get(device_id)
            if state is not None:
                state.sync_sent += 1

    def _command_topic(self, device_id: str) -> str:
        return f"v1/devices/printers/{device_id}/cmd"

    def _gcode_topic(self, device_id: str) -> str:
        return f"v1/devices/printers/{device_id}/gcode"

    def _transfer_topic(self, device_id: str) -> str:
        return f"v1/devices/printers/{device_id}/transfer"

    def _debug_command_topic(self, device_id: str) -> str:
        return f"v1/devices/printers/{device_id}/debug"

    def _next_command_id(self, device_id: str) -> int:
        with self.lock:
            state = self._printer(device_id)
            return self._alloc_command_id(state)

    @staticmethod
    def _normalize_gcode(gcode: str) -> str:
        normalized_lines: list[str] = []
        for raw_line in gcode.splitlines():
            line = raw_line.strip()
            if not line:
                normalized_lines.append("")
                continue

            comment_idx = line.find(";")
            if comment_idx >= 0:
                body = line[:comment_idx]
                comment = line[comment_idx:]
            else:
                body = line
                comment = ""

            tokens = body.split()
            if tokens:
                tokens[0] = tokens[0].upper()
                for i in range(1, len(tokens)):
                    token = tokens[i]
                    if len(token) >= 2 and token[0].isalpha() and token[1] in "0123456789+-.":
                        tokens[i] = token[0].upper() + token[1:]
                body = " ".join(tokens)

            normalized_lines.append((body + comment).strip())
        return "\n".join(normalized_lines)

    def _publish_gcode_command(self, device_id: str, gcode: str, force: bool) -> int | None:
        if not self.mqtt_connected:
            self._log(f"[TX:{self._short_id(device_id)}] skip gcode: mqtt not connected")
            return None
        cmd_id = self._next_command_id(device_id)
        env = gcode_pb2.GcodeCommandEnvelope()
        env.command_id = int(cmd_id)
        env.force = bool(force)
        env.gcode = self._normalize_gcode(str(gcode))
        binary = env.SerializeToString()
        topic = self._gcode_topic(device_id)
        info = self.client.publish(topic, payload=binary, qos=1)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            self._log(f"[TX:{self._short_id(device_id)}] publish failed rc={info.rc} gcode id={cmd_id}")
            return None
        with self.lock:
            state = self._printer(device_id)
            state.pending_commands[cmd_id] = {
                "command": "GCODE_FORCE" if force else "GCODE",
                "kwargs": {"gcode": gcode, "force": force},
                "ts": int(time.time()),
            }
        self._log(
            f"[TX:{self._short_id(device_id)}] topic={topic} cmd={'GCODE_FORCE' if force else 'GCODE'} id={cmd_id} qos=1 len={len(binary)}"
        )
        return cmd_id

    def _publish_transfer_chunk(
        self,
        device_id: str,
        transfer_id: int,
        chunk_index: int,
        data: bytes,
        last: bool,
    ) -> int | None:
        if not self.mqtt_connected:
            self._log(f"[TX:{self._short_id(device_id)}] skip transfer chunk: mqtt not connected")
            return None
        cmd_id = self._next_command_id(device_id)
        env = transfer_pb2.TransferChunkEnvelope()
        env.command_id = int(cmd_id)
        env.transfer_id = int(transfer_id)
        env.chunk_index = int(chunk_index)
        env.last = bool(last)
        env.data = data
        env.has_crc32 = True
        env.crc32 = int(zlib.crc32(data) & 0xFFFFFFFF)
        binary = env.SerializeToString()
        topic = self._transfer_topic(device_id)
        info = self.client.publish(topic, payload=binary, qos=1)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            self._log(f"[TX:{self._short_id(device_id)}] publish failed rc={info.rc} transfer chunk id={cmd_id}")
            return None
        self._log(
            f"[TX:{self._short_id(device_id)}] topic={topic} cmd=TRANSFER_CHUNK id={cmd_id} transfer_id={transfer_id} idx={chunk_index} last={last} len={len(data)}"
        )
        return cmd_id

    def _stream_transfer_chunks(self, device_id: str, transfer_id: int, local_path: Path, chunk_size: int = 1024) -> None:
        try:
            total = local_path.stat().st_size
        except OSError as exc:
            self._log(f"[TX:{self._short_id(device_id)}] failed stat for chunk stream {local_path}: {exc}")
            return
        self._log(
            f"[TX:{self._short_id(device_id)}] streaming transfer chunks transfer_id={transfer_id} local={local_path} size={total}"
        )
        try:
            with local_path.open("rb") as handle:
                idx = 0
                sent = 0
                while True:
                    chunk = handle.read(chunk_size)
                    if not chunk:
                        if idx == 0:
                            self._publish_transfer_chunk(device_id, transfer_id, idx, b"", True)
                        break
                    sent += len(chunk)
                    last = sent >= total
                    self._publish_transfer_chunk(device_id, transfer_id, idx, chunk, last)
                    idx += 1
                    if last:
                        break
            self._log(f"[TX:{self._short_id(device_id)}] transfer chunk stream done transfer_id={transfer_id}")
        except OSError as exc:
            self._log(f"[TX:{self._short_id(device_id)}] failed chunk stream local={local_path}: {exc}")

    def _queue_transfer_command(self, device_id: str, command_name: str, kwargs: dict[str, Any]) -> None:
        if command_name not in self.TRANSFER_COMMANDS:
            self._publish_command(device_id, command_name, kwargs)
            return
        with self.lock:
            state = self._printer(device_id)
            state.sync_queue.append({"command": command_name, "kwargs": dict(kwargs)})
            queue_len = len(state.sync_queue)
        self._log(
            f"[UI:{self._short_id(device_id)}] transfer queued cmd={command_name} queue={queue_len}"
        )
        self._maybe_dispatch_sync(device_id)

    def _handle_sync_response(
        self, state: PrinterState, event_payload: dict[str, Any], pending: dict[str, Any] | None
    ) -> None:
        event_name = str(event_payload.get("event") or "")
        if pending and pending.get("command") in self.TRANSFER_COMMANDS:
            if event_name == "TRANSFER_INFO":
                kwargs = pending.get("kwargs", {})
                local_path_raw = kwargs.get("__local_path") if isinstance(kwargs, dict) else None
                if isinstance(local_path_raw, str) and pending.get("command") == "START_INLINE_DOWNLOAD":
                    local_path = Path(local_path_raw)
                    threading.Thread(
                        target=self._stream_transfer_chunks,
                        args=(state.device_id, int(event_payload.get("command_id", 0)), local_path),
                        daemon=True,
                    ).start()
            if event_name in {"REJECTED", "FAILED"}:
                state.sync_inflight = False
                state.sync_failed += 1
            # TRANSFER_INFO means transfer started; keep inflight=True until terminal event.
        elif event_name in {"TRANSFER_FINISHED", "TRANSFER_ABORTED", "TRANSFER_STOPPED"}:
            state.sync_inflight = False
            if event_name == "TRANSFER_FINISHED":
                state.sync_done += 1
            else:
                state.sync_failed += 1

    def _on_message(self, _client: mqtt.Client, _userdata: object, msg: mqtt.MQTTMessage) -> None:
        topic = msg.topic if isinstance(msg.topic, str) else msg.topic.decode("utf-8", errors="replace")
        payload = bytes(msg.payload)
        try:
            device_id = topic_device_id(topic)
        except ValueError:
            self._log(f"[RX] ignore topic={topic}")
            return

        with self.lock:
            state = self._printer(device_id)
            state.last_seen = time.time()

            if topic.endswith("/data/online"):
                online = payload.decode("utf-8", errors="replace").strip() == "1"
                state.online = online
                self._log(f"[RX:{self._short_id(device_id)}] online={online}")
                return

            if topic.endswith("/event"):
                envelope = decode_event(payload)
                event_json = event_to_legacy_json(envelope)
                event_name = event_json.get("event", "UNKNOWN")
                state.last_event_type = str(event_name)
                state.last_event = event_json
                pending = self._mark_command_response(state, event_json)
                self._handle_sync_response(state, event_json, pending)
                self._apply_file_event(state, event_json)
                self._apply_state_projection(state, event_json)
                self._log(f"[RX:{self._short_id(device_id)}] event={event_name} bytes={len(payload)}")
                dispatch_needed = bool(state.sync_queue) and not state.sync_inflight
                next_device_id = state.device_id
                if dispatch_needed:
                    # Dispatch outside callback lock to keep UI reactive.
                    pass
                else:
                    next_device_id = ""
            else:
                next_device_id = ""
            if topic.endswith("/event"):
                if next_device_id:
                    self._maybe_dispatch_sync(next_device_id)
                return

            if "/data/" in topic or topic.endswith("/dialog"):
                telemetry_json = self._decode_ascii_data_topic(topic, payload)
                if telemetry_json:
                    state.last_telemetry_delta = telemetry_json
                    state.last_telemetry.update(telemetry_json)
                    self._log(
                        f"[RX:{self._short_id(device_id)}] telemetry keys={','.join(sorted(telemetry_json.keys()))} bytes={len(payload)}"
                    )
                else:
                    self._log(f"[RX:{self._short_id(device_id)}] unhandled data topic={topic} bytes={len(payload)}")
                return

            if topic.endswith("/cmd"):
                try:
                    cmd = command_pb2.CommandEnvelope()
                    cmd.ParseFromString(payload)
                    payload_name = cmd.WhichOneof("payload")
                    self._log(
                        f"[BUS:{self._short_id(device_id)}] cmd_topic command_id={cmd.command_id} payload={payload_name} bytes={len(payload)}"
                    )
                except Exception as exc:
                    self._log(
                        f"[BUS:{self._short_id(device_id)}] cmd_topic undecodable bytes={len(payload)} err={exc}"
                    )
                return

            if topic.endswith("/gcode"):
                try:
                    cmd = gcode_pb2.GcodeCommandEnvelope()
                    cmd.ParseFromString(payload)
                    self._log(
                        f"[BUS:{self._short_id(device_id)}] gcode_topic command_id={cmd.command_id} force={cmd.force} bytes={len(payload)}"
                    )
                except Exception as exc:
                    self._log(
                        f"[BUS:{self._short_id(device_id)}] gcode_topic undecodable bytes={len(payload)} err={exc}"
                    )
                return

            if topic.endswith("/transfer"):
                try:
                    chunk = transfer_pb2.TransferChunkEnvelope()
                    chunk.ParseFromString(payload)
                    self._log(
                        f"[BUS:{self._short_id(device_id)}] transfer_topic command_id={chunk.command_id} transfer_id={chunk.transfer_id} idx={chunk.chunk_index} last={chunk.last} bytes={len(chunk.data)}"
                    )
                except Exception as exc:
                    self._log(
                        f"[BUS:{self._short_id(device_id)}] transfer_topic undecodable bytes={len(payload)} err={exc}"
                    )
                return

            if topic.endswith("/debug"):
                try:
                    dbg = debug_pb2.DebugMessageEnvelope()
                    dbg.ParseFromString(payload)
                    self._log(
                        f"[DBG:{self._short_id(device_id)}] command_id={dbg.command_id} level={dbg.level} source={dbg.source} message={dbg.message}"
                    )
                except Exception as exc:
                    self._log(
                        f"[DBG:{self._short_id(device_id)}] undecodable bytes={len(payload)} err={exc}"
                    )
                return

            if topic.endswith("/telemetry"):
                telemetry = decode_telemetry(payload)
                telemetry_json = telemetry_to_legacy_json(telemetry)
                state.last_telemetry_delta = telemetry_json
                state.last_telemetry.update(telemetry_json)
                self._log(
                    f"[RX:{self._short_id(device_id)}] telemetry keys={','.join(sorted(telemetry_json.keys()))} bytes={len(payload)}"
                )
                return

            self._log(f"[RX:{self._short_id(device_id)}] unhandled topic={topic} bytes={len(payload)}")

    def _alloc_command_id(self, state: PrinterState) -> int:
        state.command_seq += 1
        return state.command_seq

    def _publish_command(self, device_id: str, command_name: str, kwargs: dict[str, Any]) -> int | None:
        if not self.mqtt_connected:
            self._log(f"[TX:{self._short_id(device_id)}] skip cmd={command_name}: mqtt not connected")
            return None
        with self.lock:
            state = self._printer(device_id)
            cmd_id = self._alloc_command_id(state)
        binary = build_command_from_json(cmd_id, command_name, kwargs)
        topic = self._command_topic(device_id)
        info = self.client.publish(topic, payload=binary, qos=1)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            self._log(
                f"[TX:{self._short_id(device_id)}] publish failed rc={info.rc} cmd={command_name} id={cmd_id}"
            )
            return None
        with self.lock:
            state = self._printer(device_id)
            state.pending_commands[cmd_id] = {
                "command": command_name,
                "kwargs": kwargs,
                "ts": int(time.time()),
            }
        self._log(
            f"[TX:{self._short_id(device_id)}] topic={topic} cmd={command_name} id={cmd_id} qos=1 len={len(binary)} kwargs={kwargs}"
        )
        return cmd_id

    def _expire_pending(self) -> None:
        now = int(time.time())
        with self.lock:
            for state in self.printers.values():
                expired = [
                    (cmd_id, pending)
                    for cmd_id, pending in state.pending_commands.items()
                    if (now - int(pending.get("ts", now))) >= self.pending_timeout_s
                ]
                for cmd_id, pending in expired:
                    state.pending_commands.pop(cmd_id, None)
                    self._log(
                        f"[ACK:{self._short_id(state.device_id)}] timeout cmd_id={cmd_id} command={pending.get('command')}"
                    )

    def _list_printers(self) -> list[str]:
        with self.lock:
            return sorted(self.printers.keys())

    def _selected(self) -> PrinterState | None:
        with self.lock:
            if self.selected_device_id is None:
                return None
            return self.printers.get(self.selected_device_id)

    def _set_selected(self, device_id: str) -> bool:
        with self.lock:
            if device_id not in self.printers:
                return False
            self.selected_device_id = device_id
            return True

    def _handle_user_command(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        try:
            tokens = shlex.split(line)
        except ValueError as exc:
            self._log(f"[UI] parse error: {exc}")
            return
        if not tokens:
            return
        cmd = tokens[0].lower()

        if cmd in {"quit", "exit"}:
            self.running = False
            return

        if cmd == "help":
            self._log(
                "[UI] commands: help, printers, select <id>, files, info, state, job, jobid <id>, transfer, stop_transfer, file <path>, mkdir <path>, rm <path[/]>, start <path>, pause, resume, stop, ready, unready, idle, reset, token <v>, cancel_obj <id>, uncancel_obj <id>, dialog <id> <button>, setv <property> <value> [tool], gcode <line>, gcodef <line>, home [X|Y|Z|XY|XZ|YZ|XYZ], jog <axis> <delta> [feed], dl_inline <local_file>|<path size [team hash]>, dl_connect <local_file>|<path size [team hash]>, dl_enc <path> <size> <key_hex> <iv_hex> [port], sync_push [local_dir] [remote_root], send <NAME> [json_kwargs], quit"
            )
            return

        if cmd == "printers":
            ids = self._list_printers()
            self._log(f"[UI] printers={ids}")
            return

        if cmd == "select":
            if len(tokens) < 2:
                self._log("[UI] usage: select <device_id>")
                return
            target = tokens[1]
            if self._set_selected(target):
                self._log(f"[UI] selected={target}")
            else:
                self._log(f"[UI] unknown printer {target}")
            return

        selected = self._selected()
        if selected is None:
            self._log("[UI] no printer selected")
            return
        device_id = selected.device_id

        if cmd == "files":
            path = tokens[1] if len(tokens) > 1 else "/usb"
            self._publish_command(device_id, "SEND_FILE_INFO", {"path": path})
            with self.lock:
                paths = sorted(self.printers[device_id].files.keys())
            self._log(f"[UI:{device_id}] mirrored_files(path={path})={paths}")
            return

        if cmd == "info":
            self._publish_command(device_id, "SEND_INFO", {})
            return
        if cmd == "state":
            self._publish_command(device_id, "SEND_STATE_INFO", {})
            return
        if cmd == "job":
            self._publish_command(device_id, "SEND_JOB_INFO", {})
            return
        if cmd == "jobid":
            if len(tokens) < 2:
                self._log("[UI] usage: jobid <job_id>")
                return
            self._publish_command(device_id, "SEND_JOB_INFO", {"job_id": int(tokens[1])})
            return
        if cmd == "transfer":
            self._publish_command(device_id, "SEND_TRANSFER_INFO", {})
            return
        if cmd == "stop_transfer":
            self._publish_command(device_id, "STOP_TRANSFER", {})
            return
        if cmd == "file":
            path = "/usb"
            if len(tokens) >= 2:
                path = tokens[1]
            self._publish_command(device_id, "SEND_FILE_INFO", {"path": path})
            return
        if cmd == "mkdir":
            if len(tokens) < 2:
                self._log("[UI] usage: mkdir </usb/path>")
                return
            self._publish_command(device_id, "CREATE_FOLDER", {"path": tokens[1]})
            return
        if cmd == "rm":
            if len(tokens) < 2:
                self._log("[UI] usage: rm </usb/path>")
                return
            path = tokens[1]
            command = "DELETE_FOLDER" if path.endswith("/") else "DELETE_FILE"
            self._publish_command(device_id, command, {"path": path.rstrip("/")})
            return
        if cmd == "start":
            path = "/usb"
            if len(tokens) >= 2:
                path = tokens[1]
            self._publish_command(device_id, "START_PRINT", {"path": path})
            return
        if cmd == "pause":
            self._publish_command(device_id, "PAUSE_PRINT", {})
            return
        if cmd == "resume":
            self._publish_command(device_id, "RESUME_PRINT", {})
            return
        if cmd == "stop":
            self._publish_command(device_id, "STOP_PRINT", {})
            return
        if cmd == "ready":
            self._publish_command(device_id, "SET_PRINTER_READY", {})
            return
        if cmd == "unready":
            self._publish_command(device_id, "CANCEL_PRINTER_READY", {})
            return
        if cmd == "idle":
            self._publish_command(device_id, "SET_IDLE", {})
            return
        if cmd == "reset":
            self._publish_command(device_id, "RESET_PRINTER", {})
            return
        if cmd == "token":
            if len(tokens) < 2:
                self._log("[UI] usage: token <value>")
                return
            self._publish_command(device_id, "SET_TOKEN", {"token": tokens[1]})
            return
        if cmd == "cancel_obj":
            if len(tokens) < 2:
                self._log("[UI] usage: cancel_obj <id>")
                return
            self._publish_command(device_id, "CANCEL_OBJECT", {"id": int(tokens[1])})
            return
        if cmd == "uncancel_obj":
            if len(tokens) < 2:
                self._log("[UI] usage: uncancel_obj <id>")
                return
            self._publish_command(device_id, "UNCANCEL_OBJECT", {"id": int(tokens[1])})
            return
        if cmd == "dialog":
            if len(tokens) < 3:
                self._log("[UI] usage: dialog <dialog_id> <button>")
                return
            self._publish_command(
                device_id,
                "DIALOG_ACTION",
                {"dialog_id": int(tokens[1]), "button": tokens[2]},
            )
            return
        if cmd == "setv":
            if len(tokens) < 3:
                self._log("[UI] usage: setv <property> <value> [tool_index]")
                return
            raw_value = tokens[2]
            try:
                value: Any = json.loads(raw_value)
            except json.JSONDecodeError:
                value = raw_value
            kwargs: dict[str, Any] = {"property": tokens[1], "value": value}
            if len(tokens) >= 4:
                kwargs["tool_index"] = int(tokens[3])
            self._publish_command(device_id, "SET_VALUE", kwargs)
            return
        if cmd == "gcode":
            gcode_line = line[len(tokens[0]) :].strip()
            if not gcode_line:
                self._log("[UI] usage: gcode <gcode_line_or_multiline_escaped>")
                return
            gcode_line = gcode_line.replace("\\n", "\n").replace("\\r", "\r")
            self._publish_gcode_command(device_id, gcode_line, force=False)
            return
        if cmd == "gcodef":
            gcode_line = line[len(tokens[0]) :].strip()
            if not gcode_line:
                self._log("[UI] usage: gcodef <gcode_line_or_multiline_escaped>")
                return
            gcode_line = gcode_line.replace("\\n", "\n").replace("\\r", "\r")
            self._publish_gcode_command(device_id, gcode_line, force=True)
            return
        if cmd == "home":
            axes = tokens[1].upper() if len(tokens) > 1 else ""
            if axes and any(ch not in {"X", "Y", "Z"} for ch in axes):
                self._log("[UI] usage: home [X|Y|Z|XY|XZ|YZ|XYZ]")
                return
            gcode_line = f"G28 {axes}".strip()
            self._publish_gcode_command(device_id, gcode_line, force=False)
            return
        if cmd == "jog":
            if len(tokens) < 3:
                self._log("[UI] usage: jog <axis:X|Y|Z> <delta> [feed]")
                return
            axis = tokens[1].upper()
            if axis not in {"X", "Y", "Z"}:
                self._log("[UI] usage: jog <axis:X|Y|Z> <delta> [feed]")
                return
            try:
                delta = float(tokens[2])
                feed = float(tokens[3]) if len(tokens) > 3 else 3000.0
            except ValueError:
                self._log("[UI] jog numeric parse error")
                return
            gcode_line = f"G91\\nG1 {axis}{delta} F{feed}\\nG90"
            self._publish_gcode_command(device_id, gcode_line, force=False)
            return
        if cmd == "dl_inline":
            kwargs = self._resolve_download_kwargs(tokens, command_name="START_INLINE_DOWNLOAD")
            if kwargs is None:
                return
            self._queue_transfer_command(device_id, "START_INLINE_DOWNLOAD", kwargs)
            return
        if cmd == "dl_connect":
            kwargs = self._resolve_download_kwargs(tokens, command_name="START_CONNECT_DOWNLOAD")
            if kwargs is None:
                return
            self._queue_transfer_command(device_id, "START_CONNECT_DOWNLOAD", kwargs)
            return
        if cmd == "dl_enc":
            if len(tokens) < 5:
                self._log("[UI] usage: dl_enc <path> <size> <key_hex> <iv_hex> [port]")
                return
            kwargs = {
                "path": tokens[1],
                "orig_size": int(tokens[2]),
                "key": tokens[3],
                "iv": tokens[4],
            }
            if len(tokens) > 5:
                kwargs["port"] = int(tokens[5])
            self._queue_transfer_command(device_id, "START_ENCRYPTED_DOWNLOAD", kwargs)
            return
        if cmd == "sync_push":
            default_local_dir = self.storage_root / device_id
            local_arg = tokens[1] if len(tokens) > 1 else "."
            if local_arg == ".":
                local_dir = default_local_dir
            else:
                local_dir = Path(local_arg).expanduser().resolve()
            remote_root = tokens[2] if len(tokens) > 2 else "/usb"
            if not local_dir.exists() or not local_dir.is_dir():
                self._log(
                    f"[UI] local_dir not found: {local_dir} (expected default: {default_local_dir})"
                )
                return
            self._log(f"[UI:{device_id}] sync source={local_dir} -> {remote_root}")
            files = sorted(
                [
                    p
                    for p in local_dir.rglob("*")
                    if p.is_file() and not self._is_hidden_or_ignored_path(p, local_dir)
                ]
            )
            if not files:
                self._log(f"[UI] no files to sync in {local_dir}")
                return
            prepared: list[dict[str, Any]] = []
            skipped = 0
            for file_path in files:
                rel = file_path.relative_to(local_dir).as_posix()
                remote_path = f"{remote_root.rstrip('/')}/{rel}"
                kwargs = self._download_kwargs_from_local_file(file_path, remote_path=remote_path)
                if kwargs is None:
                    skipped += 1
                    continue
                kwargs["__local_path"] = str(file_path)
                prepared.append(kwargs)
            with self.lock:
                state = self.printers[device_id]
                state.sync_queue.extend({"command": "START_INLINE_DOWNLOAD", "kwargs": kwargs} for kwargs in prepared)
            self._maybe_dispatch_sync(device_id)
            self._log(
                f"[UI:{device_id}] sync_push queued {len(prepared)} files (skipped={skipped}) from {local_dir} to {remote_root}"
            )
            return
        if cmd == "send":
            if len(tokens) < 2:
                self._log("[UI] usage: send <COMMAND_NAME> [json_kwargs]")
                return
            command_name = tokens[1]
            kwargs: dict[str, Any] = {}
            if len(tokens) >= 3:
                kwargs_blob = " ".join(tokens[2:])
                try:
                    parsed = json.loads(kwargs_blob)
                    if not isinstance(parsed, dict):
                        self._log("[UI] kwargs must be JSON object")
                        return
                    kwargs = parsed
                except json.JSONDecodeError as exc:
                    self._log(f"[UI] invalid kwargs json: {exc}")
                    return
            try:
                if command_name in self.TRANSFER_COMMANDS:
                    self._queue_transfer_command(device_id, command_name, kwargs)
                else:
                    self._publish_command(device_id, command_name, kwargs)
            except Exception as exc:
                self._log(f"[UI] failed to send command: {exc}")
            return

        self._log(f"[UI] unknown command: {cmd}")

    @staticmethod
    def _sha256_file(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            while True:
                chunk = handle.read(64 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
        return digest.hexdigest()

    def _download_kwargs_from_local_file(self, local_path: Path, remote_path: str | None = None) -> dict[str, Any] | None:
        if not local_path.exists() or not local_path.is_file():
            self._log(f"[UI] local file not found: {local_path}")
            return None
        if remote_path is None:
            remote_path = f"/usb/{local_path.name}"
        try:
            stat = local_path.stat()
        except OSError as exc:
            self._log(f"[UI] cannot stat file {local_path}: {exc}")
            return None
        if int(stat.st_size) <= 0:
            self._log(f"[UI] skipping empty file {local_path}")
            return None
        try:
            file_hash = self._sha256_file(local_path)
        except OSError as exc:
            self._log(f"[UI] cannot read file {local_path}: {exc}")
            return None
        return {
            "path": remote_path,
            "orig_size": int(stat.st_size),
            "team_id": 0,
            "hash": file_hash,
        }

    def _resolve_download_kwargs(self, tokens: list[str], command_name: str) -> dict[str, Any] | None:
        if len(tokens) < 2:
            self._log(f"[UI] usage: {command_name.lower()} <local_file> OR <path> <size> [team_id] [hash]")
            return None

        if len(tokens) == 2:
            local_path = Path(tokens[1]).expanduser().resolve()
            kwargs = self._download_kwargs_from_local_file(local_path)
            if kwargs is None:
                self._log("[UI] auto mode expects existing local file path")
            else:
                kwargs["__local_path"] = str(local_path)
            return kwargs

        try:
            return {
                "path": tokens[1],
                "orig_size": int(tokens[2]),
                "team_id": int(tokens[3]) if len(tokens) > 3 else 0,
                "hash": tokens[4] if len(tokens) > 4 else "mock-hash",
            }
        except ValueError as exc:
            self._log(f"[UI] invalid numeric argument: {exc}")
            return None

    def _snapshot_for_ui(self) -> tuple[list[str], list[str]]:
        with self.lock:
            raw_logs = [line for _, line in self.logs]
            printer_ids = sorted(self.printers.keys())
            selected = self.printers.get(self.selected_device_id) if self.selected_device_id else None

        right_lines: list[str] = []
        right_lines.append("## Control")
        right_lines.append(f"Selected: {self._short_id(selected.device_id) if selected else '-'}")
        right_lines.append(
            f"Printers: {', '.join(self._short_id(pid) for pid in printer_ids) if printer_ids else '-'}"
        )
        right_lines.append("")
        if selected is not None:
            right_lines.append("## Status")
            right_lines.append(f"Online: {selected.online}")
            right_lines.append(f"Last Event: {selected.last_event_type}")
            right_lines.append(f"Pending Cmd: {len(selected.pending_commands)}")
            right_lines.append(f"Job: {selected.job if selected.job else '-'}")
            right_lines.append(f"Transfer: {selected.transfer if selected.transfer else '-'}")
            right_lines.append(
                f"Sync: queued={len(selected.sync_queue)} inflight={selected.sync_inflight} sent={selected.sync_sent} done={selected.sync_done} failed={selected.sync_failed}"
            )
            right_lines.append("## Telemetry Full")
            if selected.last_telemetry:
                right_lines.extend(self._two_col_kv_lines(selected.last_telemetry))
            else:
                right_lines.append("  -")
            right_lines.append("## Telemetry Delta")
            if selected.last_telemetry_delta:
                for key, value in sorted(selected.last_telemetry_delta.items()):
                    right_lines.append(f"  {key}: {value}")
            else:
                right_lines.append("  -")
            right_lines.append("## Recent Event")
            if selected.last_event:
                for line in self._event_lines(selected.last_event):
                    right_lines.append(f"  {line}")
            else:
                right_lines.append("  -")
        right_lines.append("")
        right_lines.append("Hint: type `help` for command list.")
        return raw_logs, right_lines

    def _two_col_kv_lines(self, data: dict[str, Any]) -> list[str]:
        items = sorted(data.items())
        half = (len(items) + 1) // 2
        left_col = items[:half]
        right_col = items[half:]
        out: list[str] = []
        for idx in range(max(len(left_col), len(right_col))):
            left = left_col[idx] if idx < len(left_col) else ("", "")
            right = right_col[idx] if idx < len(right_col) else ("", "")
            left_text = f"{left[0]}: {left[1]}" if left[0] else ""
            right_text = f"{right[0]}: {right[1]}" if right[0] else ""
            if right_text:
                out.append(f"  {left_text} | {right_text}")
            else:
                out.append(f"  {left_text}")
        return out

    def _event_lines(self, payload: dict[str, Any], max_lines: int = 10) -> list[str]:
        lines: list[str] = []
        list_preview_limit = 3

        def append_node(key: str, value: Any, indent: int) -> None:
            pad = " " * indent
            if isinstance(value, dict):
                if not value:
                    lines.append(f"{pad}{key}: {{}}")
                    return
                lines.append(f"{pad}{key}:")
                for sub_key, sub_value in sorted(value.items()):
                    append_node(str(sub_key), sub_value, indent + 4)
                return
            if isinstance(value, list):
                if not value:
                    lines.append(f"{pad}{key}: []")
                    return
                # Avoid exploding right pane on verbose arrays like FILE_INFO children.
                lines.append(f"{pad}{key}: [{len(value)} items]")
                preview = value[:list_preview_limit]
                for idx, item in enumerate(preview):
                    if isinstance(item, dict):
                        name = item.get("display_name") or item.get("name") or "?"
                        item_type = item.get("type", "?")
                        size = item.get("size")
                        suffix = f", size={size}" if size is not None else ""
                        lines.append(f"{pad}    - [{idx}] {name} ({item_type}{suffix})")
                    else:
                        lines.append(f"{pad}    - [{idx}] {item}")
                hidden = len(value) - len(preview)
                if hidden > 0:
                    lines.append(f"{pad}    ... (+{hidden} more)")
                return
            lines.append(f"{pad}{key}: {value}")

        for key, value in sorted(payload.items()):
            append_node(str(key), value, 0)

        if len(lines) <= max_lines:
            return lines
        hidden = len(lines) - max_lines
        return lines[:max_lines] + [f"... (+{hidden} more)"]

    def _wrap_lines(self, lines: list[str], width: int) -> list[str]:
        if width <= 1:
            return [""]
        wrapped: list[str] = []
        for line in lines:
            parts = wrap(line, width=width, replace_whitespace=False, drop_whitespace=False)
            if parts:
                wrapped.extend(parts)
            else:
                wrapped.append("")
        return wrapped

    def _line_color(self, line: str) -> int:
        if "[RX:" in line:
            return 2
        if "[TX:" in line:
            return 3
        if "[ACK:" in line:
            return 4
        if "[UI]" in line:
            return 5
        if "[MQTT]" in line or "[SYS]" in line:
            return 6
        return 1

    def _right_line_style(self, line: str) -> int:
        if line.startswith("## "):
            return curses.color_pair(2) | curses.A_BOLD
        if line.startswith("Hint:"):
            return curses.color_pair(6) | curses.A_BOLD
        if line.startswith("  "):
            return curses.color_pair(1)
        return curses.color_pair(4) | curses.A_BOLD

    def _draw_curses(self, stdscr: Any, command_line: str) -> None:
        stdscr.erase()
        rows, cols = stdscr.getmaxyx()
        if rows < 10 or cols < 40:
            stdscr.addstr(0, 0, "Terminal too small (min 40x10).", curses.color_pair(6))
            stdscr.refresh()
            return

        left_w = max(30, cols // 2)
        right_x = left_w + 1
        right_w = cols - right_x - 1
        body_top = 2
        input_row = rows - 2
        body_rows = max(1, input_row - body_top)

        logs, right_lines = self._snapshot_for_ui()
        logs_wrapped = self._wrap_lines(logs, left_w - 2)
        logs_view = logs_wrapped[-body_rows:]

        stdscr.addstr(0, 0, " MQTT + Protobuf Mock Server ", curses.color_pair(6) | curses.A_BOLD)
        stdscr.addstr(1, 0, " Logs".ljust(left_w), curses.color_pair(6))
        stdscr.addstr(1, right_x, " State / Actions".ljust(right_w), curses.color_pair(6))

        for y in range(body_top, input_row):
            try:
                stdscr.addch(y, left_w, "|", curses.color_pair(6))
            except curses.error:
                pass

        for idx, line in enumerate(logs_view):
            y = body_top + idx
            if y >= input_row:
                break
            stdscr.addnstr(y, 0, line, left_w - 1, curses.color_pair(self._line_color(line)))

        for idx, line in enumerate(right_lines[:body_rows]):
            y = body_top + idx
            stdscr.addnstr(y, right_x, line, right_w, self._right_line_style(line))

        stdscr.hline(input_row - 1, 0, "-", cols, curses.color_pair(6))
        prompt = f"> {command_line}"
        stdscr.addnstr(input_row, 0, prompt, cols - 1, curses.color_pair(5) | curses.A_BOLD)
        stdscr.move(input_row, min(cols - 1, len(prompt)))
        stdscr.refresh()

    def _run_curses_ui(self) -> None:
        def _ui(stdscr: Any) -> None:
            curses.curs_set(1)
            curses.start_color()
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_WHITE, -1)
            curses.init_pair(2, curses.COLOR_CYAN, -1)
            curses.init_pair(3, curses.COLOR_YELLOW, -1)
            curses.init_pair(4, curses.COLOR_GREEN, -1)
            curses.init_pair(5, curses.COLOR_MAGENTA, -1)
            curses.init_pair(6, curses.COLOR_BLUE, -1)
            stdscr.nodelay(True)
            stdscr.timeout(150)
            buffer = ""

            while self.running:
                self._expire_pending()
                self._draw_curses(stdscr, buffer)
                try:
                    key = stdscr.get_wch()
                except curses.error:
                    continue

                if key in ("\n", "\r"):
                    self._handle_user_command(buffer)
                    buffer = ""
                    continue
                if key in ("\x03",):  # Ctrl+C
                    self.running = False
                    continue
                if key in ("\x7f", "\b", "\x08") or key == curses.KEY_BACKSPACE:
                    buffer = buffer[:-1]
                    continue
                if isinstance(key, str) and key.isprintable():
                    buffer += key
                    continue

        curses.wrapper(_ui)

    def _stream_logs_no_ui(self) -> None:
        with self.lock:
            pending = [(idx, line) for idx, line in self.logs if idx > self._last_printed_log_id]
        for idx, line in pending:
            print(line)
            self._last_printed_log_id = idx

    def _wait_for_printer(self, target_device_id: str | None, timeout_s: float) -> str | None:
        deadline = time.monotonic() + timeout_s
        while self.running and time.monotonic() < deadline:
            with self.lock:
                printer_ids = sorted(self.printers.keys())
            if target_device_id:
                if target_device_id in printer_ids:
                    return target_device_id
            elif printer_ids:
                return printer_ids[0]
            time.sleep(0.2)
        return None

    def _run_one_shot(self) -> int:
        selected = self._wait_for_printer(self.args.device_id, self.args.wait_printer_timeout)
        if selected is None:
            print("[ERROR] no printer available for one-shot command", file=sys.stderr)
            return 3
        self._set_selected(selected)
        for command_line in self.args.command:
            self._log(f"[SYS] one-shot: {command_line}")
            self._handle_user_command(command_line)
            time.sleep(0.1)
        self._stream_logs_no_ui()
        return 0

    def run(self) -> int:
        self._log(
            f"[SYS] starting mock-server host={self.args.host} port={self.args.port} user={self.args.username} insecure={self.args.insecure}"
        )
        self._ensure_mock_server_acls()
        self.client.connect_async(self.args.host, self.args.port, keepalive=self.args.keepalive)
        self.client.loop_start()

        if not self.connected_event.wait(timeout=self.args.connect_timeout):
            print(
                f"[ERROR] MQTT connect timeout after {self.args.connect_timeout}s to {self.args.host}:{self.args.port}",
                file=sys.stderr,
            )
            self.client.loop_stop()
            return 2
        if self.connect_error is not None:
            print(f"[ERROR] MQTT connect failed: {self.connect_error}", file=sys.stderr)
            self.client.loop_stop()
            return 2

        self._log("[SYS] mock server started")
        exit_code = 0
        try:
            if self.args.command:
                exit_code = self._run_one_shot()
                self.running = False
            elif self.args.no_ui or not sys.stdin.isatty() or not sys.stdout.isatty():
                self._log("[SYS] running in no-ui mode")
                while self.running:
                    self._expire_pending()
                    self._stream_logs_no_ui()
                    time.sleep(0.2)
            else:
                # UI loop owns refresh cadence; pending timeout still needs updates.
                self._run_curses_ui()
        except KeyboardInterrupt:
            self.running = False
        except Exception as exc:
            print(f"[ERROR] ui failed: {exc}", file=sys.stderr)
            self.running = False

        self._log("[SYS] shutting down")
        self._stream_logs_no_ui()
        self.client.loop_stop()
        self.client.disconnect()
        return exit_code


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MQTT + Protobuf mock server for CONNECT2 bring-up.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8883)
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", required=True)
    parser.add_argument("--ca-file", type=Path, default=Path("certs/ca.crt"))
    parser.add_argument("--insecure", action="store_true")
    parser.add_argument("--keepalive", type=int, default=30)
    parser.add_argument("--client-id", default="mock-server")
    parser.add_argument("--storage-root", type=Path, required=True)
    parser.add_argument("--connect-timeout", type=float, default=8.0)
    parser.add_argument("--no-ui", action="store_true")
    parser.add_argument("--device-id", default=None, help="target printer for --command mode")
    parser.add_argument("--wait-printer-timeout", type=float, default=10.0, help="seconds to wait for printer in --command mode")
    parser.add_argument("--command", action="append", default=[], help="execute command line and exit (repeatable)")
    parser.add_argument("--log-level", default=os.getenv("LOG_LEVEL", "INFO"))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=getattr(logging, str(args.log_level).upper(), logging.INFO))
    app = MockServerApp(args)

    def _stop(_sig: int, _frame: Any) -> None:
        app.running = False

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    return app.run()


if __name__ == "__main__":
    raise SystemExit(main())
