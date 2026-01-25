# ZADANI — MQTT místo WebSocketů v Prusa‑Firmware‑Buddy

## Cíl
- Nahradit stávající komunikaci Prusa Connect přes WebSockety za MQTT.
- Zachovat **TLS podporu** ve firmware (kvůli bezpečnosti a dalším částem FW), ale **odstranit Connect/WebSocket funkcionalitu** z buildu.
- Postupně rozšířit MQTT tak, aby pokryl kompletní funkcionalitu původního Connectu.

## Proč
- Uvolnit paměť a místo ve firmware pro MQTT experimenty.
- Zjednodušit a sjednotit komunikaci přes MQTT místo WebSocket/HTTP.

## Co už je hotové (stav k dnešku)
- MQTT‑C knihovna přidaná do repa: `lib/Middlewares/Third_Party/MQTT-C`.
- Nová architektura **CONNECT2**:
  - `src/connect2/` obsahuje samostatného klienta a task, bez závislosti na Connect/WebSocket.
  - CONNECT a CONNECT2 jsou **mutually exclusive** (CONNECT2=ON, CONNECT=OFF).
- Nový modul `src/common/mqtt/`:
  - `mqtt_client.*` — C++ wrapper nad MQTT‑C (connect/step/disconnect + keepalive ping).
  - `mqtt_transport.*` — transport (plain + TLS), přepíná timeouts (handshake vs IO).
  - `tls/` — MbedTLS TLS klient (přesunuto z Connectu, bez proxy).
  - `mqtt_pal.*` — PAL pro MQTT‑C (send/recv přes Transport).
  - **Lokální kopie** `connection.*` a `socket.*` (původní `http/` beze změn, MQTT má vlastní verze).
- TLS v firmware:
  - Používá se **MbedTLS**.
  - TLS klient je implementovaný v `src/common/mqtt/tls/tls.cpp/.hpp`.
  - CA certy: `src/common/mqtt/tls/certificate.h` (DER), nebo custom cert z `/internal/connect/connect.der`.
  - MbedTLS je napojený na lwIP sockety přes `src/common/mqtt/tls/net_sockets.cpp`.
  - HW entropy pro MbedTLS: `src/common/mqtt/tls/hardware_rng.cpp`.
- Konfigurace přes USB:
  - `Load Settings` v UI umí načíst **CONNECT2** config z `prusa_printer_settings.ini`.
  - Pokud `custom_cert=true`, kopíruje `/usb/connect.der` → `/internal/connect/connect.der`.

## Jak je kód strukturovaný (relevantní části)
- `src/connect2/`:
  - `client.*` — state machine, backoff, reconnect + network gating (vyžaduje alespoň jedno IF up + IPv4).
  - `config.*` — čte `config_store` (host/port/tls/custom_cert/enabled) + import z INI.
  - `telemetry.*` — MQTT telemetrie (polling) s filtrováním jen změněných hodnot, PoC parity.
- `src/common/mqtt/`:
  - `mqtt_client.*` — wrapper nad MQTT‑C (keepalive ping, logy connect/sync).
  - `mqtt_transport.*` — TLS/Plain transport, timeout switching (handshake 60s, IO 5s).
  - `tls/` — MbedTLS handshake + verify.
  - `connection.*`, `socket.*` — lokální MQTT verze socket/connection (abychom nezasahovali do HTTP/Connect).
- `src/common/http/`:
  - Zůstává beze změn a používá ho původní Connect.
  - CONNECT2 má dočasně připojené vybrané zdroje z `src/connect/` (MarlinPrinter/printer_common/hostname/printer)
    kvůli telemetrii; TODO: vytáhnout společný kód do `src/common/`.

## Jak vypnout Connect/WebSocket (nastavení)
- V CMake options:
  - `CONNECT` → generuje `BUDDY_ENABLE_CONNECT()` (Connect klient).
  - `CONNECT2` → generuje `BUDDY_ENABLE_CONNECT2()` (Connect2 klient).
  - `WEBSOCKET` → generuje `WEBSOCKET()` (WS path).
- V aktuální branch: **CONNECT2=ON, CONNECT=OFF, WEBSOCKET=OFF**.

## Jak přidáváme TLS do MQTT (aktuální návrh)
- TLS je součást `src/common/mqtt/` a je **transport‑agnostic** vůči CONNECT2.
- CONNECT2 pouze řídí stavový automat a volá `mqtt_client.connect/step/disconnect`.
- MQTT klient zodpovídá za:
  - otevření TCP (plain nebo TLS),
  - handshake a ověření certifikátu,
  - `mqtt_sync()` pumpování,
  - keepalive ping (fallback když se v MQTT‑C neprojeví).

## Klíčové opravy a poznatky (TLS/MQTT)
- `server_cert missing` v logu neznamená, že TLS selhal — MbedTLS build neuchovává peer cert.
- IO timeout pro MQTT musí být krátký (IO 5s), jinak `mqtt_sync()` blokuje a broker odpojí klienta (keepalive timeout).
- Po pádu socketu je nutné **ihned zavřít transport** (TLS + socket), jinak se generují stovky chyb a plní se send buffer.
- `EAGAIN/EWOULDBLOCK` není chyba — proto se v MQTT socketu neloguje.

## Co je potřeba dodělat (TODO)
- [x] Vypnout Connect/WebSocket v buildu (nastavení v CMake).
- [x] Vybudovat CONNECT2 + MQTT klient s TLS transportem.
- [x] Umožnit konfiguraci MQTT brokeru (host/port, TLS on/off, custom cert) přes `config_store`.
- [x] Zajistit workflow USB → config_store (import z INI + kopie `connect.der` do `/internal/connect/`).
- [x] Ošetřit timeouts, keepalive pingy a reconnect logiku v MQTT vrstvě.
- [x] Implementovat MQTT telemetrii a zachovat PoC topics/payloads/QoS/retain.
- [ ] Ověřit heap/stack usage (buffers, TLS handshake, mqtt_sync).
- [ ] Postupně doplnit MQTT funkcionalitu, aby pokryla Connect (telemetrie, příkazy, job info, atd.).
- [ ] Rozhodnout finální cert flow:
  - buď hostname + SAN(dNSName),
  - nebo IP bez SAN (mbedTLS v tomto FW IP SAN **neověřuje**).

## Poznámky pro Codex AI (pracovní kontext)
- Pracovní repo: `Prusa-Firmware-Buddy`
- Aktuální branch: `mqtt_connect` (nová zelená louka, CONNECT2)
- TLS implementace je v `src/common/mqtt/tls`
- WebSocket a Connect jsou vypnuté, TLS musí zůstat
- Poznámka k mbedTLS: při přítomném SAN kontroluje jen **dNSName**, IP SAN ne
