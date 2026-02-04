# Connect2 MQTT Commands & Events (Current Implementation)

This document describes the commands/events currently implemented in `src/connect2`,
how they map to the original `src/connect` behavior, and how to test them over MQTT.

## Topics

- Commands (printer receives):
  - `v1/devices/printers/{SN}/cmd`
- Events (printer emits):
  - `v1/devices/printers/{SN}/event`
- Telemetry (printer emits JSON):
  - `v1/devices/printers/{SN}/data/...`

> `{SN}` is the printer serial number (e.g. `10589-3742441600009157`).

## Decoder (protobuf -> JSON)

Use the helper to decode protobuf events:

```bash
python3 tools/mqtt_info_decode.py \
  --host 192.168.1.112 \
  --port 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  --topic "v1/devices/printers/+/event" \
  --insecure --pretty
```

## Commands (implemented)

All commands are protobuf `Command` messages from `specs/connect2/command.proto`.

### SEND_INFO
- Purpose: request an `INFO` event immediately.
- Mirrors `src/connect` `SEND_INFO`.

```bash
printf 'type: SEND_INFO\ncommand_id: 1001' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

### SEND_FILE_INFO
- Purpose: request details for a file or directory on USB.
- Allowed paths: `/usb` and `/usb/...`
- Rejections: `Forbidden path`, `File not found`
- Mirrors `src/connect` `SEND_FILE_INFO`.

```bash
printf 'type: SEND_FILE_INFO\ncommand_id: 1002\npath: "/usb"' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

### SEND_JOB_INFO
- Purpose: request current job info (optional `job_id` check).
- Rejections: `No job in progress`, `Job ID doesn't match`
- Mirrors `src/connect` `SEND_JOB_INFO`.

```bash
printf 'type: SEND_JOB_INFO\ncommand_id: 2005' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

Optional job_id:

```bash
printf 'type: SEND_JOB_INFO\ncommand_id: 2006\njob_id: 180' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

### START_PRINT
- Purpose: start a print from USB.
- Checks: path allowed, file exists/valid transfer, printer accepts start.
- On success: emits `JOB_INFO`.
- On failure: emits `REJECTED` with reason.
- Mirrors `src/connect` `START_PRINT`.

```bash
printf 'type: START_PRINT\ncommand_id: 2001\npath: "/usb/BenchyRules_PLA_14m.bgcode"' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

### PAUSE_PRINT
- Purpose: pause current print.
- On success: emits `FINISHED`.
- On failure: emits `REJECTED` with `No print to pause`.
- Mirrors `src/connect` `PAUSE_PRINT`.

```bash
printf 'type: PAUSE_PRINT\ncommand_id: 2006' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

### RESUME_PRINT
- Purpose: resume a paused print.
- On success: emits `FINISHED`.
- On failure: emits `REJECTED` with `No paused print to resume`.
- Mirrors `src/connect` `RESUME_PRINT`.

```bash
printf 'type: RESUME_PRINT\ncommand_id: 2007' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

### STOP_PRINT
- Purpose: stop the current print.
- On success: emits `FINISHED`.
- On failure: emits `REJECTED` with `No print to stop`.
- Mirrors `src/connect` `STOP_PRINT`.

```bash
printf 'type: STOP_PRINT\ncommand_id: 2004' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

### RESET / RESET_PRINTER
- Purpose: reset the printer.
- Behavior: calls `reset_printer()`; if it returns, emits `REJECTED` with `Failed to reset`.
- Mirrors `src/connect` `RESET` / `RESET_PRINTER`.

```bash
printf 'type: RESET\ncommand_id: 2010' | \
  protoc --encode=Command -I specs/connect2 specs/connect2/command.proto | \
  mosquitto_pub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt \
    -t "v1/devices/printers/10589-3742441600009157/cmd" -s
```

## Events (implemented)

### INFO
- Emitted when info fingerprint changes or on `SEND_INFO`.

### FILE_INFO
- Emitted on `SEND_FILE_INFO`.
- Directory listings are **capped to 8 children** (nanopb max_count).

### FILE_CHANGED
- Emitted via `ChangedPath` when file/dir changes on USB.

### REJECTED
- Emitted on command failure with a reason string.

### JOB_INFO
- Emitted on successful `START_PRINT` or `SEND_JOB_INFO`.

### FINISHED
- Emitted as a response to PAUSE/RESUME/STOP when the command succeeds.
- Not emitted automatically on job end (same as `src/connect`).

### FAILED
- Reserved for command failures where we explicitly emit `FAILED` (future use).

### STATE_CHANGED
- Emitted when `params.state_fingerprint()` changes (same as `src/connect`).
- Note: this can be frequent even if the visible state stays `IDLE`, because the
  fingerprint also changes with dialog/state details. This is normal.

## Quick MQTT monitoring

Raw monitoring:

```bash
mosquitto_sub -h 192.168.1.112 -p 8883 --cafile /etc/mosquitto/certs/ca.crt -t '#' -v
```

Protobuf decode:

```bash
python3 tools/mqtt_info_decode.py \
  --host 192.168.1.112 \
  --port 8883 \
  --cafile /etc/mosquitto/certs/ca.crt \
  --topic 'v1/devices/printers/+/event' \
  --insecure --pretty
```

## What’s left / not yet implemented

Common `src/connect` items still missing in connect2:
- `SEND_STATE_INFO`
- `SET_PRINTER_READY`, `CANCEL_PRINTER_READY`, `SET_IDLE`
- `CANCEL_OBJECT`, `UNCANCEL_OBJECT` and `CANCELABLE_CHANGED`
- `DIALOG_ACTION`, `SET_VALUE`
- Transfer/download commands and TRANSFER_* events
- Reset commands (`RESET_PRINTER` / `RESET`)
