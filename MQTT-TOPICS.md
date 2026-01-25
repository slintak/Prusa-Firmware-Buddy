# MQTT Topics — CONNECT2 telemetry (MQTT-C PoC parity)

This document defines the **exact** MQTT topics and payloads produced by the MQTT‑C PoC
(branch `mqttc-tls-poc`). CONNECT2 must match these topics, payloads, QoS and retain flags
bit‑for‑bit.

## Topic prefixes
- `v1/devices/printers/<printer_id>/data...`
- `v1/devices/printers/<printer_id>/jobs/<job_id>/data...`
- `v1/devices/printers/<printer_id>/dialog`

`printer_id` is the OTP serial (via `otp_get_serial_nr`), or `DUMMY` if missing.

## Printer topics
- `.../online`
  - Payload: `"1"` (online) / `"0"` (offline)
  - QoS: 1
  - Retain: true
  - LWT: payload `"0"`, retained

- `.../state`
  - Payload: `printer_state::to_str(params.state.device_state)`
  - QoS: 1
  - Retain: true

- `.../axis-z`
  - Payload: float string `%.2f` (`params.pos[Z]`)
  - QoS: 0
  - Retain: true

- `.../axis-x`
  - Payload: float string `%.2f` (`params.pos[X]`) — **only if no job**
  - QoS: 0
  - Retain: false

- `.../axis-y`
  - Payload: float string `%.2f` (`params.pos[Y]`) — **only if no job**
  - QoS: 0
  - Retain: false

- `.../current-job`
  - Payload: job id (or `0`)
  - QoS: 1
  - Retain: true

- `.../job-progress`
  - Payload: `params.progress_percent`
  - QoS: 0
  - Retain: true

- `.../temp/nozzle/current`
  - Payload: float string `%.1f` (`params.slots[preferred_head()].temp_nozzle`)
  - QoS: 0
  - Retain: true

- `.../temp/nozzle/target`
  - Payload: float string `%.1f` (`params.target_nozzle`)
  - QoS: 1
  - Retain: true

- `.../temp/heatbed/current`
  - Payload: float string `%.1f` (`params.temp_bed`)
  - QoS: 0
  - Retain: true

- `.../temp/heatbed/target`
  - Payload: float string `%.1f` (`params.target_bed`)
  - QoS: 1
  - Retain: true

- `.../speed`
  - Payload: `params.print_speed`
  - QoS: 1
  - Retain: true

- `.../material`
  - Payload: `params.slots[preferred_slot()].material`
  - QoS: 1
  - Retain: true

## Job topics (only when `params.has_job`)
- `.../jobs/<job_id>/data/state`
  - Payload: same as `.../state`
  - QoS: 1
  - Retain: true

- `.../jobs/<job_id>/data/progress`
  - Payload: `params.progress_percent`
  - QoS: 0
  - Retain: true

- `.../jobs/<job_id>/data/time-remaining`
  - Payload: `params.time_to_end` (if valid)
  - QoS: 0
  - Retain: true

## Dialog topic
- `v1/devices/printers/<printer_id>/dialog`
  - Payload: `dialog_id` (uint32)
  - QoS: 2
  - Retain: true

## Telemetry cadence (PoC parity)
- `TELEMETRY_INTERVAL_MIN = 750 ms`
- `TELEMETRY_INTERVAL_LONG = 4000 ms` (idle)
- `TELEMETRY_INTERVAL_SHORT = 1000 ms` (printing)
- `TELEMETRY_INTERVAL_FULL = 5 min`
- Send only changed values (tracked via `connect_client::Tracked` + `params.telemetry_fingerprint(!printing)`).
