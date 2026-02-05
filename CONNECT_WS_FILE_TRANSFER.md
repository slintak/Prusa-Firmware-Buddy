## Přenos souboru Connect -> tiskárna (USB)

### 1) Start přenosu: START_CONNECT_DOWNLOAD / START_INLINE_DOWNLOAD

Server pošle command:

- START_CONNECT_DOWNLOAD (nebo START_INLINE_DOWNLOAD, obojí firmware bere stejně)
- Payload obsahuje:
  - path (cílová cesta na USB, např. /usb/test.gcode)
  - hash (SHA / fingerprint)
  - orig_size (velikost v bajtech)
  - team_id (Connect metadata)

Výsledek:
Tiskárna pošle eventu REJECTED, pokud:
- již nějaký přenos souboru probíhá
- soubor na dané cestě již existuje
- chyba úložiště

Pokud je vše v pořádku, firmware spustí přenos a začne čekat na data.

---

### 2) Stream dat přes websocket (T frames)

Data se posílají jako binární WS rámce typu T (transfer chunk).
Každý chunk je max 512 B (kvůli interním WS bufferům).

Firmware streamuje data přímo na USB, po částech.

---

### 3) Eventy během přenosu souboru

- TRANSFER_INFO
  Payload obsahuje:
  - size (očekávaná velikost)
  - transferred (už přeneseno)
  - progress (v %)
  - time_remaining
  - time_transferring
  - type (CONNECT / LINK)
  - path (cílová cesta)

  Eventa se posílá:
  1. Bezprostředně po zahájení přenosu
     V Planner::handle_transfer_result() se po úspěšném Start*Download
     nastaví planned_event = TransferInfo (s start_cmd_id).
  2. Na explicitní požadavek serveru
     Příkaz SEND_TRANSFER_INFO vyvolá TransferInfo event s aktuálním stavem.

- TRANSFER_FINISHED
  Přenos dokončen úspěšně.

- TRANSFER_ABORTED
  Přenos přerušen chybou (např. storage/network).
  machine_reason: STORAGE_FAILURE, NETWORK_FAILURE, OTHER.

- TRANSFER_STOPPED
  Přenos byl zastaven (např. po příkazu STOP_TRANSFER).

---

### 4) Po dokončení přenosu

Po dokončení přenosu se soubor na USB reálně vytvoří/změní,
a transfers::ChangedPath to zaznamená. Firmware pak emituje:

- FILE_INFO pokud je to nově vytvořený soubor (incident Created)
  Payload obsahuje:
  - path
  - display_name
  - type (např. FILE, FOLDER, PRINT_FI, FIRMWARE)
  - size
  - m_timestamp
  - read_only
  - children[] + file_count (pokud je to složka)

  Navíc, pokud jde o G-code a je kompletní,
  může FILE_INFO obsahovat i preview/meta bloky (renderované z GcodeExtra).

- FILE_CHANGED ve všech ostatních případech (smazání, rename, rescan, nebo změna adresáře)
  Payload obsahuje:
  - free_space (aktuální volné místo na USB)
  - new_path (u vytvoření/renamu)
  - old_path (u smazání/renamu)
  - rescan (true pokud nelze přesně určit změnu)
  - file (metadata položky):
    - name
    - display_name
    - size
    - m_timestamp
    - read_only
    - type

---

## Příkazy pro práci se soubory/složkami

### CREATE_FOLDER

- Vytvoří složku na /usb/...
- Po úspěchu -> FILE_CHANGED (incident Created, new_path), protože je to složka (is_file = false)
- Při chybě -> REJECTED (např. "Forbidden path", "Storage failure").
- Payload obsahuje cestu ke složce

### DELETE_FILE

- Smaže soubor na /usb/...
- Po úspěchu -> FILE_CHANGED (Deleted, old_path).
- Při chybě -> REJECTED (např. "Forbidden path", "File not found").
- Payload obsahuje cestu k souboru

### DELETE_FOLDER

- Smaže složku na /usb/... (typicky musí být prázdná).
- Po úspěchu -> FILE_CHANGED (Deleted, old_path).
- Při chybě -> REJECTED.
- Payload obsahuje cestu ke složce

### STOP_TRANSFER

- Zastaví probíhající přenos.
- Pokud běží přenos -> TRANSFER_STOPPED.
- Pokud přenos neběží -> REJECTED s důvodem "No transfer in progress".
- Payload je bez argumentů

---

## Související eventy k souborům

### FILE_INFO

Slouží k získání detailů o souboru/složce.

- vyvolá se příkazem SEND_FILE_INFO, nebo po dokončení downloadu souboru (pokud je to nově vytvořený soubor, is_file = true)
- pokud je target složka, obsahuje payload children[] (seznam souborů) - payload se případně streamuje přes WS, kvůli omezené velikosti bufferu.

### FILE_CHANGED

Emitue se při změně filesystemu:

- vytvoření, smazání, nebo dokončení downloadu souboru
  (smazání, rename, rescan, nebo změna adresáře)

---

## Omezení

- Connect neumí stáhnout soubory z USB zpět na server (pouze server -> printer).
- START_URL_DOWNLOAD je v docs, ale v Buddy firmware není implementovaný.

---

## File transfer diagram

```text
Connect Server                      Buddy FW (src/connect)                   USB Storage
       |                                         |                                   |
       | START_CONNECT_DOWNLOAD / START_INLINE_DOWNLOAD                              |
       | {path, hash, orig_size, team_id}        |                                   |
       |---------------------------------------->|                                   |
       |                                         |  (if cannot start)                |
       |                                         |  REJECTED                         |
       |                                         |  - Another transfer in progress   |
       |                                         |  - File already exists            |
       |                                         |  - Storage failure                |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       |                                         | TRANSFER_INFO (start_cmd_id,...)  |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       |   T frame (chunk <= 512B)               |
       |---------------------------------------->|                                   |
       |                                         | write chunk                       |
       |                                         |------------------------------->   |
       |                                         |                                   |
       |   (optional polling)                    |
       | SEND_TRANSFER_INFO                      |
       |---------------------------------------->|                                   |
       |                                         | TRANSFER_INFO (progress, path)    |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       |   (optional stop)                       |
       | STOP_TRANSFER                           |
       |---------------------------------------->|                                   |
       |                                         | if no transfer: REJECTED          |
       |                                         | "No transfer in progress"       |
       |                                         | else: TRANSFER_STOPPED            |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       |   ... repeat chunks ...                 |
       |                                         |                                   |
       |                                         | TRANSFER_FINISHED / STOPPED /     |
       |                                         | TRANSFER_ABORTED (machine_reason) |
       |                                         |  - STORAGE_FAILURE                |
       |                                         |  - NETWORK_FAILURE                |
       |                                         |  - OTHER                          |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       |                                         | file created/updated              |
       |                                         |------------------------------->   |
       |                                         | FILE_INFO if Created + is_file    |
       |                                         | FILE_CHANGED for other changes    |
       |<----------------------------------------|                                   |
```

Notes:
- TRANSFER_INFO is not periodic; it is sent only after start and when requested via SEND_TRANSFER_INFO.
- FILE_INFO vs FILE_CHANGED after transfer is decided by ChangedPath incident (Created -> FILE_INFO).

## Filesystem related commands diagram

SEND_FILE_INFO / FILE_INFO

```text
Connect Server                      Buddy FW (src/connect)                   USB Storage
       |                                         |                                   |
       | SEND_FILE_INFO {path}                   |                                   |
       |---------------------------------------->|                                   |
       |                                         | validate path (/usb/...)          |
       |                                         | stat/open dir                     |
       |                                         |------------------------------->   |
       |                                         |                                   |
       |                                         | FILE_INFO                          |
       |                                         | {path, type, size, m_timestamp,   |
       |                                         |  read_only, children[], file_count} |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       | (on error)                              | REJECTED                           |
       |                                         | "Forbidden path" / "File not found"|
       |<----------------------------------------|                                   |
```

Notes:
- If path is a directory -> children[] contains a listing (streamed).
- If path is a file -> returns metadata without children.

CREATE_FOLDER (server -> printer)

```text
Connect Server                      Buddy FW (src/connect)                   USB Storage
       |                                         |                                   |
       | CREATE_FOLDER {path}                    |                                   |
       |---------------------------------------->|                                   |
       |                                         | create directory                  |
       |                                         |------------------------------->   |
       |                                         |                                   |
       |                                         | FILE_CHANGED (Created) or         |
       |                                         | FILE_INFO (new folder)            |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       | (on error)                              | REJECTED (Forbidden path / etc.)  |
       |<----------------------------------------|                                   |
```

DELETE_FILE (server -> printer)

```text
Connect Server                      Buddy FW (src/connect)                   USB Storage
       |                                         |                                   |
       | DELETE_FILE {path}                      |                                   |
       |---------------------------------------->|                                   |
       |                                         | delete file                       |
       |                                         |------------------------------->   |
       |                                         |                                   |
       |                                         | FILE_CHANGED (Deleted, old_path)  |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       | (on error)                              | REJECTED (File not found / etc.)  |
       |<----------------------------------------|                                   |
```

DELETE_FOLDER (server -> printer)

```text
Connect Server                      Buddy FW (src/connect)                   USB Storage
       |                                         |                                   |
       | DELETE_FOLDER {path}                    |                                   |
       |---------------------------------------->|                                   |
       |                                         | delete directory (must be empty)  |
       |                                         |------------------------------->   |
       |                                         |                                   |
       |                                         | FILE_CHANGED (Deleted, old_path)  |
       |<----------------------------------------|                                   |
       |                                         |                                   |
       | (on error)                              | REJECTED (Not empty / etc.)       |
       |<----------------------------------------|                                   |
```
