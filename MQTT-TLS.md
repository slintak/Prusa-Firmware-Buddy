# MQTT-TLS — Mosquitto + FW setup (MK4/MK4S)

## Cíl
- Zprovoznit MQTT přes TLS s dočasným Mosquitto serverem.
- FW používá TLS přes `buddy::mqtt::tls` (MbedTLS) a bere nastavení z `config_store`
  (přes `prusa_printer_settings.ini` + `connect.der`).

## Mosquitto konfigurace (TLS-only na portu 1883)
```
listener 1883 0.0.0.0
protocol mqtt

cafile /etc/mosquitto/certs/ca.crt
certfile /etc/mosquitto/certs/server.crt
keyfile /etc/mosquitto/certs/server.key

tls_version tlsv1.2
ciphers ECDHE-ECDSA-AES128-GCM-SHA256

allow_anonymous true
require_certificate false
```

## Generování certifikátů (CA + server)
V adresáři `mosquitto-tls/`:

1) CA klíč + cert:
```
openssl ecparam -genkey -name prime256v1 -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days 3650 \
  -subj "/C=CZ/O=Dev CA/CN=Dev Mosquitto CA" -out ca.crt
```

2) Server klíč + CSR:
```
openssl ecparam -genkey -name prime256v1 -out server.key
openssl req -new -key server.key -subj "/C=CZ/O=Dev/CN=10.2.0.248" -out server.csr
```

3) Server extensions (`server.ext`) — **bez SAN** (viz poznámky níže):
```
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
```

4) Podpis server certu CA:
```
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 825 -extfile server.ext
```

5) DER export CA pro FW:
```
openssl x509 -in ca.crt -outform der -out ca.der
```

## FW — jak dostat CA do tiskárny
FW očekává **CA cert** jako DER v:
```
/internal/connect/connect.der
```

Aktuální cesta je stejná jako u původního Connectu. Není to „čtení přímo z USB“ —
nejdřív se musí cert zkopírovat do `/internal/connect/`.

1) Na USB dej:
   - `connect.der` (z `ca.der`)
   - `prusa_printer_settings.ini`
2) `prusa_printer_settings.ini` obsah:
```
[service::connect]
tls=true
custom_cert=true
```
3) V UI použij „Load Settings“ (import z USB), což:
   - načte INI do `config_store`
   - zkopíruje `/usb/connect.der` → `/internal/connect/connect.der`
4) **Poznámka:** bez importu z USB zůstane `custom_cert=false` a cert se nepoužije.

## Jak ověřit cert ve FW
MQTT logy při startu (Serial / USB logging):
- `custom_cert=true path=/internal/connect/connect.der`
- `custom_ca sha256=... size=...`
- `builtin_ca[...] sha256=... size=...`
- `server_cert sha256=... size=...`
- `tls_verify_flags=0x... (text)`

Hash ověříš na PC:
```
openssl dgst -sha256 mosquitto-tls/ca.der
```

## Důležité poznámky k TLS a mbedTLS
- `server_cert missing` v logu neznamená, že TLS selhal — build mbedTLS neuchovává peer cert.
- mbedTLS v tomto FW při přítomném SAN kontroluje pouze **dNSName**, **IP SAN ne**.
  Pokud SAN existuje, mbedTLS ignoruje CN.
  Řešení: cert **bez SAN** (jen `CN=10.2.0.248`) nebo SAN s DNS jménem a používat hostname.

## MQTT keepalive a timeouts
- MQTT handshake potřebuje delší timeout (60 s) kvůli pomalému FW.
- Po navázání spojení se IO timeout přepíná na kratší hodnotu (5 s),
  aby `mqtt_sync()` neblokoval a posílal se keepalive.
- Keepalive pingy posíláme explicitně, pokud je spojení idle.

## Co jsme zkoušeli a jaké byly problémy
- `custom_cert=false` → FW bral vestavěný CA (Prusa), TLS fail.
  - Důvod: config z USB se nenačetl / cert nebyl importovaný do `/internal/connect/`.
  - Fix: použít „Load Settings“ v UI (import z USB).

- `tls_error=Memory` při handshaku:
  - Důvod: MbedTLS entropy context byl alokovaný pouze z lwIP 512B poolu.
  - Fix: fallback na `malloc_fallible` (FreeRTOS heap).

- `tls_mbedtls_code=0x-9984`:
  - `-0x2700` = `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`.
  - Fix: logování `tls_verify_flags`, přegenerovaný server cert bez SAN.

- `tls_verify_flags=0x4` = `MBEDTLS_X509_BADCERT_CN_MISMATCH`:
  - Výše popsaný problém s SAN vs CN.

- Keepalive timeout (Mosquitto „exceeded timeout“):
  - Příčina: dlouhý IO timeout blokoval `mqtt_sync()`.
  - Fix: po handshaku přepnout IO timeout na 5 s + explicitní pingy.

- Pád serveru (errno 128, ssl write failed, send buffer full):
  - Příčina: socket/TLS nebyly ihned zavřeny po network error, MQTT‑C dál posílal.
  - Fix: při `Error::Network` v transportu zavřít TLS+socket;
    socket zároveň na `ENOTCONN/ECONNRESET/EPIPE` ihned zavře fd.

## Jak zapnout logování na USB
Firmware logy jdou přes USB CDC pouze v „logging“ režimu.
Přepnutí se dělá nastavením baudrate na **57600**.
- 57600 → logging
- cokoliv jiného → Marlin serial

Alternativa: „Save Logs To File“ v menu Tools (logy na USB disk).

## Ověření serveru (openssl)
```
openssl s_client -connect 127.0.0.1:1883 \
  -servername 10.2.0.248 \
  -tls1_2 \
  -cipher ECDHE-ECDSA-AES128-GCM-SHA256 \
  -CAfile mosquitto-tls/ca.crt
```
