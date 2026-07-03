# 01 · Architektur & Datenfluss (🇩🇪)

Wie alles zusammenhängt und **was in welche Richtung fließt**. Drei Links: Zähler↔Reader (IR),
Reader↔Gateway (LoRa 868 MHz), Gateway↔Cloud (MQTTS), plus App↔Gateway (BLE) für das Setup.

## Komponenten
| Knoten | Chip | Rolle |
|---|---|---|
| **Zähler** | — | Stromzähler mit optischer/IR‑Schnittstelle (DLMS/OBIS) |
| **Reader** | BAT32G135 (ARM Cortex‑M0+) + SX1262 | Liest den Zähler, sendet Energie per LoRa |
| **Gateway** | ESP32‑C3 (RISC‑V) + SX1262 (Ra‑03SCH) | LoRa ↔ WLAN/BLE ↔ Cloud |
| **Cloud** | AWS IoT (Hersteller) **oder dein MQTTS‑Broker** | Provisioning + Telemetrie + OTA |
| **App** | heyOBI | BLE‑Setup: WLAN, Zertifikate, Sensor‑Bind |

> **Firmware 1.2.x** erweitert das: ein Gateway bedient jetzt **mehrere Sensoren gleichzeitig** und ergänzt
> **Smart‑Outlets** (Relais + Leistungsmessung). Die Telemetrie wechselte auf `schema_version: 2`
> (`paired_devices[]` mit `device`‑Typ, `upload_interval`, `outlet/…`‑Topics), und Downlink‑Kommandos sind
> **pro Gerät** (sie tragen die Ziel‑`uuid`). Siehe
> [03-cloud-api.md](03-cloud-api.md#downlink-kommandos-cloud--gerät). Die Links unten sind über die
> Versionen unverändert.

## Gesamt‑Datenfluss
```mermaid
flowchart TD
    MET["⚡ Zähler"] -->|"IR: OBIS 1.8.x / 2.8.x / 16.7.0"| RDR["📟 Reader BAT32G135"]
    RDR -->|"LoRa-Uplink: Energie-Frames<br/>(cmd 17/19/22/23, 1/2/3)"| BRG["📶 Gateway ESP32-C3"]
    BRG -->|"LoRa-Downlink: Scan/Bind,<br/>OTA-Blöcke (cmd 12/71)"| RDR
    BRG -->|"MQTTS 8883 (TLS)"| CLD[("☁️ MQTTS-Broker<br/>deiner oder Hersteller")]
    CLD -->|"Provisioning, Shadow,<br/>OTA firmware-data"| BRG
    PH["📱 App"] -.->|"BLE ABF2 (TEA+JSON):<br/>Status/WifiSet/SensorBind/SetTMPCertificate"| BRG
    BRG -.->|"BLE ABF1 (TEA+JSON): Antworten"| PH
```

## Wer sendet was (Richtungen)

### LoRa (Reader ↔ Gateway, 868 MHz)
```mermaid
sequenceDiagram
    participant R as Reader (BAT32G135)
    participant B as Gateway (ESP32-C3)
    Note over R,B: Frame = handle(3B) + typ/cmd(1B) + payload, XOR-Key = (h0+h1+h2) & 0xFF
    R->>B: cmd 32 — ECDH-Pubkey (Handshake-Gate)
    B->>R: cmd 32 — Gateway-Pubkey (setzt key-ready)
    R->>B: cmd 17 — Announce (UUID + RSSI)
    B->>R: Scan/Bind-Antwort
    R->>B: cmd 19/22/23 — Energiedaten (Version, Leistung, OBIS)
    R->>B: cmd 21 — OTA-Block-Anfrage (Offset)
    B->>R: cmd 12 Metadaten / cmd 71 64-Byte-Firmware-Block
```
Details & Payload‑Layouts: [03-lora-protokoll.md](03-lora-protokoll.md).

### Cloud (Gateway ↔ MQTTS)
```mermaid
sequenceDiagram
    participant B as Gateway
    participant C as MQTTS-Broker (deiner)
    B->>C: TLS-Connect (Claim-Cert), MQTT CONNECT
    B->>C: PUBLISH $aws/certificates/create/json
    C-->>B: .../accepted — permanentes Cert
    B->>C: PUBLISH $aws/provisioning-templates/TEMPLATE/provision/json
    C-->>B: .../accepted — thingName
    B->>C: Reconnect mit permanentem Cert
    B->>C: PUBLISH $aws/rules/.../state (Telemetrie/Heartbeat)
    C-->>B: (Downlink) cmd/.../upload-interval-change-request, ota/firmware-update-request
    Note over B,C: OTA-Pull — C sendet 23-Byte-Offer, B fordert firmware-data-request(offset), C sendet firmware-data-response-Chunk
```
Topics & Self‑Hosting: [04-eigene-cloud.md](04-eigene-cloud.md).

### BLE (App ↔ Gateway, Setup)
```mermaid
sequenceDiagram
    participant P as App / Tool
    participant B as Gateway
    Note over P,B: GATT ABF0 · ABF2=Write(TX) · ABF1=Notify(RX) · Payload = TEA(JSON-Frames)
    P->>B: ABF2  StatusRequest
    B-->>P: ABF1  Status (uuid, fw, wifi, ...)
    P->>B: ABF2  UnbindRequest  (alten Besitzer ZUERST loesen)
    P->>B: ABF2  SensorScanRequest dann SensorBindRequest (uuid)
    P->>B: ABF2  WifiSetRequest (ssid, password)
    P->>B: ABF2  SetTMPCertificateRequest (url, caPem, certPem, privateKey)  (ZULETZT)
```
Codec + Command‑Liste: [03-ble-protokoll.md](03-ble-protokoll.md).

## Transporte & das interne „vsocket"
Eine Framing‑Schicht (**vsocket**), gemultiplext über eine *Pipe‑ID* und verteilt über eine *Protokoll*‑Nummer:

| Pipe (Transport) | Protokoll | erreichbar über |
|---|---|---|
| 1 | BLE‑JSON (TEA) | BLE ABF2 |
| 0 | Proto 2 — Management / OTA‑aus‑Cloud | MQTT / BLE Pipe 0 |
| 2 | Proto 254 — Klartext‑Config | **UART0‑Konsole** |
| (LoRa) | Proto 0 — Reader‑Commands | 868‑MHz‑Funk |

Deshalb tauchen dieselben OTA‑/Command‑Handler auf mehreren Links auf. Referenz:
[03-firmware-layout.md](03-firmware-layout.md).
