Přehled PoC

  - buddy::mqttc_poc::tick() je voláno z hlavního „tick“ cyklu ve lib/Marlin/Marlin/src/Marlin.cpp:513, takže se MQTT PoC obsluhuje
    periodicky při běhu firmware.
  - PoC běží hned, jakmile je detekovaný běžící Wi‑Fi netif (NETDEV_NETIF_UP) a uplyne krátké „stabilizační“ okno ~1.5 s (viz
    wifi_ready() v src/common/mqttc_poc/mqttc_poc.cpp).
  - Pokud není spojení, PoC provádí opakované pokusy s backoffem (5 s) a udržuje spojení přes mqtt_sync().

  Použitá knihovna (MQTT‑C)

  - PoC používá knihovnu MQTT‑C jako externí subrepo; fixní verze je v1.1.6 (viz lib/Middlewares/Third_Party/MQTT-C/.gitrepo).
  - Do firmware se kompilují pouze mqtt.c + lokální PAL wrapper (viz src/common/mqttc_poc/CMakeLists.txt).

  PAL (Platform Abstraction Layer)

  - MQTT‑C je portován přes vlastní PAL: src/common/mqttc_poc/mqttc_pal.h a src/common/mqttc_poc/mqttc_pal.c.
  - PAL mapuje čas, mutexy, socket handle a implementuje mqtt_pal_sendall() / mqtt_pal_recvall() nad lwIP.

  Kdy se posílá telemetrie

  - Telemetrie se posílá jen po navázání spojení a v cyklu tick().
  - Odeslání je řízené změnami hodnot + periodickým „full“ refresh:
      - min interval mezi publish je ~750 ms,
      - běžný interval je 1 s (při tisku) / 4 s (netiskne se),
      - „full“ refresh je vynucen každých 5 minut, i bez změn.
        Vše je řízeno v telemetry_due() / publish_telemetry() v src/common/mqttc_poc/mqttc_poc.cpp.

  Co se posílá (telemetrie a zdroj dat)

  - Zdroj dat je connect_client::MarlinPrinter a jeho params() (viz src/common/mqttc_poc/mqttc_poc.cpp).
  - Hlavní témata (suffixy) a obsah:
      - /online – dostupnost zařízení (retain).
      - /state – stav tiskárny (z printer_state::to_str(...)).
      - /axis-z, /axis-x, /axis-y – aktuální pozice os.
      - /current-job, /job-progress, /time-remaining – informace o jobu.
      - /temp/nozzle/current, /temp/nozzle/target, /temp/heatbed/current, /temp/heatbed/target.
      - /speed – rychlost tisku.
      - /material – tiskový materiál.
      - /dialog – ID dialogu.
  - Telemetrie se posílá, když se změnila hodnota, plus periodický full refresh (5 min).


MQTT PoC (bez TLS) konzumuje průměrně ~6.6 KB heap paměti

