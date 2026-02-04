#!/usr/bin/env python3
"""
Decode connect2 protobuf INFO events from MQTT.

The script subscribes to the event topic, attempts to parse the payload as
InfoEvent, and prints JSON to stdout. If decoding fails, it prints a hexdump
to help inspect unexpected payloads.
"""
import argparse
import binascii
import json
import os
import sys


def ensure_proto_module(proto_py, proto_src, include_dir):
    # Generate *_pb2.py at runtime if it is missing.
    if os.path.exists(proto_py):
        if os.path.getmtime(proto_py) >= os.path.getmtime(proto_src):
            return
    try:
        from grpc_tools import protoc  # type: ignore
    except Exception as exc:
        raise SystemExit(
            f"Missing {proto_py}. Install grpcio-tools or pre-generate it. Error: {exc}"
        )
    args = [
        "protoc",
        f"-I{include_dir}",
        f"--python_out={os.path.dirname(proto_py)}",
        proto_src,
    ]
    if protoc.main(args) != 0:
        raise SystemExit("protoc failed while generating python module")


def hexdump(data: bytes, width: int = 16) -> str:
    # Basic hexdump helper for unknown payloads.
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hex_part = binascii.hexlify(chunk).decode("ascii")
        hex_part = " ".join(hex_part[j : j + 2] for j in range(0, len(hex_part), 2))
        ascii_part = "".join((chr(b) if 32 <= b < 127 else ".") for b in chunk)
        lines.append(f"{i:08x}  {hex_part:<{width*3}}  {ascii_part}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode connect2 INFO protobuf events from MQTT")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8883)
    parser.add_argument("--cafile", required=True)
    parser.add_argument("--topic", default="v1/devices/printers/+/event")
    parser.add_argument("--username")
    parser.add_argument("--password")
    parser.add_argument("--insecure", action="store_true", help="Disable TLS cert verification")
    parser.add_argument("--pretty", action="store_true", help="Pretty-print JSON output")
    args = parser.parse_args()

    # Resolve repo-relative paths so it can be run from any working directory.
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    proto_src = os.path.join(repo_root, "specs", "connect2", "info_event.proto")
    proto_py = os.path.join(repo_root, "tools", "info_event_pb2.py")

    ensure_proto_module(proto_py, proto_src, os.path.dirname(proto_src))

    from google.protobuf.json_format import MessageToDict  # type: ignore
    import info_event_pb2  # type: ignore

    try:
        import paho.mqtt.client as mqtt  # type: ignore
    except Exception as exc:
        raise SystemExit(f"Missing paho-mqtt. Install it via pip. Error: {exc}")

    client = mqtt.Client()
    if args.username is not None:
        client.username_pw_set(args.username, args.password)

    client.tls_set(ca_certs=args.cafile)
    if args.insecure:
        client.tls_insecure_set(True)

    def on_connect(_client, _userdata, _flags, rc):
        if rc != 0:
            print(f"MQTT connect failed: rc={rc}")
            return
        print(f"Connected. Subscribing to {args.topic}")
        _client.subscribe(args.topic)

    def on_message(_client, _userdata, msg):
        data = msg.payload
        decoders = [
            ("InfoEvent", info_event_pb2.InfoEvent, "INFO", "data"),
            ("JobInfoEvent", info_event_pb2.JobInfoEvent, "JOB_INFO", "job_info"),
            ("FinishedEvent", info_event_pb2.FinishedEvent, "FINISHED", None),
            ("FailedEvent", info_event_pb2.FailedEvent, "FAILED", None),
            ("StateChangedEvent", info_event_pb2.StateChangedEvent, "STATE_CHANGED", None),
            ("FileInfoEvent", info_event_pb2.FileInfoEvent, "FILE_INFO", "file_info"),
            ("FileChangedEvent", info_event_pb2.FileChangedEvent, "FILE_CHANGED", "file_changed"),
            ("RejectedEvent", info_event_pb2.RejectedEvent, "REJECTED", "rejected"),
        ]
        # Try decoders in order; first one that matches the "event" field wins.
        for name, cls, expected_event, expected_field in decoders:
            event = cls()
            try:
                event.ParseFromString(data)
                # Convert protobuf to JSON-friendly dict for easy inspection.
                decoded = MessageToDict(event, preserving_proto_field_name=True)
                if not isinstance(decoded, dict):
                    continue
                if decoded.get("event") != expected_event:
                    continue
                if expected_field is not None and expected_field not in decoded:
                    continue
                if args.pretty:
                    payload = json.dumps(decoded, ensure_ascii=False, indent=2, sort_keys=True)
                else:
                    payload = json.dumps(decoded, ensure_ascii=False)
                header = f"{name}"
                if "command_id" in decoded:
                    header += f" command_id={decoded.get('command_id')}"
                print(f"\n{msg.topic}\n{header}\n{payload}")
                return
            except Exception:
                continue
        print(f"\n{msg.topic}\n<decode failed> len={len(data)}")
        print(hexdump(data))

    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(args.host, args.port, keepalive=60)
    client.loop_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
