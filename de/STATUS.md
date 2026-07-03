# Status, Firmware‑Abdeckung & Roadmap (🇩🇪)

## Für welche Firmware dieses Repo ist
- **Gateway (ESP32‑C3):** analysiert über **1.0.0 – 1.2.1** und den separaten **31.0.0 – 34.0.0**‑Zweig.
- **Protokoll‑Details und Funktionsadressen stammen aus `1.0.2`** (dem primär reversten Image). Die Funde
  wurden stichprobenartig gegen die anderen Versionen geprüft und gelten dort auch (gleiches TEA, gleiches
  UART‑Config‑Protokoll, gleiches LoRa‑Framing, gleiches Fleet‑Provisioning).
- **Reader (BAT32G135):** aus jedem Gateway‑Build extrahierbar; Funde stammen vom `31.0.0`‑Reader. (Der
  `1.2.x`‑Reader ist ein größerer Build — siehe [02-hardware.md](02-hardware.md).)
- 🔒 **Keine Firmware‑Binaries im Repo** (SUMEC/OBI‑Copyright, aus Sicherheitsgründen entfernt und
  git‑ignoriert) — eigenes Dump mitbringen, siehe [firmware.md](firmware.md).

> ⚠️ **Firmware ist ein bewegliches Ziel.** Ein künftiges OTA kann UUIDs, Command‑IDs, Payload‑Layouts,
> Timing ändern oder echte Krypto hinzufügen. Alles hier bezieht sich auf die obigen Versionen — vor dem
> Verlassen auf ein Detail gegen die eigene Einheit gegenprüfen.

## OTA‑Signatur‑Status (Stand der neuesten analysierten Version)
- Das Gateway‑Selbst‑Update (MQTT‑OTA) trägt nur einen **Integritäts‑Hash (16 Byte, MD5‑artig)** — **keine
  kryptografische Signatur**. Keine Secure‑Boot‑Signaturprüfung gefunden, keine Flash‑Encryption‑Strings im
  App‑Image.
- **Folge:** ein über die **eigene Cloud** per OTA ausgeliefertes Firmware‑Image wird akzeptiert — genau das
  macht **Custom‑Firmware** trotz gesperrtem Bootloader machbar
  ([firmware-layout.md](03-firmware-layout.md)).
- Kann sich ändern: eine künftige Version könnte **signiertes OTA / Secure Boot** einführen und den
  Custom‑Firmware‑Weg schließen.

## Noch offen / Mithilfe erwünscht 🚧
Zwei große Teile sind **noch nicht fertig** — die Hauptarbeit:

1. ~~**Echte Stromdaten über MQTT.**~~ ✅ **Erledigt** — die Telemetrie ist JSON und End‑to‑End dekodiert
   (statisch + an echtem Gerät bestätigt). Die Zählerwerte laufen über
   `$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state` (und `dt/…/state/live`) als
   `{uuid, bridge_uuid, …, timestamp, rssi, battery, energy, negative_energy, power}`. Volles Schema +
   Live‑Beispiel: [03-cloud-api.md](03-cloud-api.md#telemetrie-payloads-dekodiert--an-echtem-gerät-bestätigt).
   Offen nur noch: die **Einheit** von `energy` gegen den 1.8.0‑Stand eines echten Zählers festnageln.
2. **Mit dem Gerät über MQTT sprechen.** *In Arbeit — zwei Downlink‑Kommandos vollständig reversed &
   verifiziert.* Der Downlink/Command‑Pfad (Protokoll 2 über Pipe 0) hat jetzt:
   - **Reader‑Upload‑Intervall‑Änderung** — `{sensor_upload_interval, session_id}` auf
     `cmd/…/sensor/<UUID>/upload-interval-change-request` publishen; `mqtt_set_upload_interval` (Proto 2
     cmd 6) übernimmt es und antwortet auf `…-response`. Broker: `mqtts_server.py --set-interval N`.
   - **OTA‑Firmware‑Update** — der 23‑Byte‑Offer + JSON‑Data‑Request + 9‑Byte‑Header‑Data‑Response‑Chunk‑
     Ablauf ist vollständig gemappt (`mqtt_ota_file_info` / `mqtt_ota_data`), byte‑genau gegen einen echten
     Cloud‑Mitschnitt und einen End‑to‑End‑Selbsttest bestätigt. Das Selbst‑Update ist **unsigniert**, also
     ein funktionierender **Custom‑Firmware**‑Weg. Broker: `mqtts_server.py --ota-firmware fw.bin`. Siehe
     [OTA](03-cloud-api.md#ota) / [Firmware flashen](04-eigene-cloud.md#eigene-firmware-flashen).

   Beide dokumentiert unter [Downlink‑Kommandos](03-cloud-api.md#downlink-kommandos-cloud--gerät).
   Noch offen: die restlichen Command‑Payloads (allgemein Status/Config/Control).

**Bekannte Einschränkung (kein Bug):** einen Reader hinzuzufügen/koppeln geht **nur über BLE** — es gibt
kein MQTT/Cloud‑Kommando zum Scannen oder Binden, und das BLE des Gateways ist nur im Setup‑Fenster aktiv.
Das Pairing muss also in der BLE‑Setup‑Session passieren (eingebaut in `ble_provision.py --pair-sensor`) —
siehe [07-reader-koppeln.md](07-reader-koppeln.md).

Ebenfalls offen (kleiner): die Mehr‑Reader‑`SensorScan`‑Enumeration bestätigen
([07-reader-koppeln.md](07-reader-koppeln.md)) und die exakten SX1262‑RF‑Parameter erfassen
([02-hardware.md](02-hardware.md)).

Beiträge willkommen. Gute Einstiegspunkte: [eigene Cloud + Broker‑Log](04-eigene-cloud.md),
[MQTT‑Topics](03-firmware-layout.md), [LoRa‑Energie‑Payload](03-lora-protokoll.md).
