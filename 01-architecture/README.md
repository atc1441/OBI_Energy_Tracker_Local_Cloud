# 01 · Architecture & Data Flow

How the pieces connect and **what travels in each direction**. Three links: meter↔reader (IR),
reader↔bridge (LoRa 868 MHz), bridge↔cloud (MQTTS), plus phone↔bridge (BLE) for setup.

## Components

| Node | Chip | Role |
|---|---|---|
| **Meter** | — | Electricity meter with optical/IR port (DLMS/OBIS) |
| **Reader** | BAT32G135 (ARM Cortex-M0+) + SX1262 | Reads the meter, sends energy over LoRa |
| **Bridge** | ESP32-C3 (RISC-V) + SX1262 (Ra-03SCH) | LoRa ↔ WiFi/BLE ↔ cloud gateway |
| **Cloud** | AWS IoT (vendor) **or your MQTTS broker** | Provisioning + telemetry + OTA |
| **Phone** | heyOBI app | BLE setup: WiFi, certs, sensor bind |

> **Firmware 1.2.x** widened this: a bridge now handles **several sensors at once** and adds **smart
> outlets** (relay + power metering). Telemetry moved to `schema_version: 2` (`paired_devices[]` with a
> `device` type, `upload_interval`, `outlet/…` topics), and downlink commands became **per-device**
> (they carry the target `uuid`). See [cloud-api.md](../03-reverse-engineering/cloud-api.md#downlink-commands-cloud--device).
> The links below are unchanged across versions.

## End-to-end data flow

```mermaid
flowchart TD
    subgraph Field["Field (per sensor)"]
      MET["⚡ Meter"] -->|"IR: OBIS 1.8.x / 2.8.x / 16.7.0"| RDR["📟 Reader BAT32G135"]
    end

    RDR -->|"LoRa uplink: energy frames<br/>(cmd 17/19/22/23, 1/2/3)"| BRG["📶 Bridge ESP32-C3"]
    BRG -->|"LoRa downlink: scan/bind,<br/>OTA blocks (cmd 12/71)"| RDR

    subgraph Home["Home / LAN"]
      BRG
      AP["🛜 Your WiFi AP"]
      BRG --- AP
    end

    AP -->|"MQTTS 8883 (TLS)"| CLD[("☁️ MQTTS broker<br/>yours or vendor")]
    CLD -->|"provisioning, shadow,<br/>OTA firmware-data"| BRG

    PH["📱 Phone"] -.->|"BLE ABF2 write (TEA+JSON):<br/>Status/WifiSet/SensorBind/SetTMPCertificate"| BRG
    BRG -.->|"BLE ABF1 notify (TEA+JSON):<br/>responses"| PH
```

## Who sends what (directions)

### LoRa (reader ↔ bridge, 868 MHz)
```mermaid
sequenceDiagram
    participant R as Reader (BAT32G135)
    participant B as Bridge (ESP32-C3)
    Note over R,B: frame = handle(3B) + type/cmd(1B) + payload, XOR key = (h0+h1+h2) & 0xFF
    R->>B: cmd 32 — ECDH pubkey (handshake gate)
    B->>R: cmd 32 — bridge pubkey (sets key-ready)
    R->>B: cmd 17 — announce (UUID + RSSI)
    B->>R: scan/bind response
    R->>B: cmd 19/22/23 — energy data (softver, power, OBIS)
    R->>B: cmd 1/2/3 — bound power/energy
    R->>B: cmd 21 — OTA block request (offset)
    B->>R: cmd 12 metadata / cmd 71 64-byte firmware block
```
Details & payload byte layouts: [../03-reverse-engineering/lora-protocol.md](../03-reverse-engineering/lora-protocol.md).

### Cloud (bridge ↔ MQTTS)
```mermaid
sequenceDiagram
    participant B as Bridge
    participant C as MQTTS broker (yours)
    B->>C: TLS connect (claim cert), MQTT CONNECT
    B->>C: PUBLISH $aws/certificates/create/json
    C-->>B: .../accepted — permanent ("consistent") cert
    B->>C: PUBLISH $aws/provisioning-templates/TEMPLATE/provision/json
    C-->>B: .../accepted — thingName
    B->>C: reconnect with permanent cert
    B->>C: PUBLISH $aws/rules/.../state (telemetry / heartbeat)
    C-->>B: (downlink) cmd/.../upload-interval-change-request, ota/firmware-update-request
    Note over B,C: OTA pull — C sends 23-byte offer, B requests firmware-data-request(offset), C sends firmware-data-response chunk
```
Topics and the self-host setup: [../04-connect-your-own-cloud/README.md](../04-connect-your-own-cloud/README.md).

### BLE (phone ↔ bridge, setup)
```mermaid
sequenceDiagram
    participant P as Phone / tool
    participant B as Bridge
    Note over P,B: GATT ABF0 · ABF2=write(TX) · ABF1=notify(RX) · payload = TEA(JSON frames)
    P->>B: ABF2  StatusRequest
    B-->>P: ABF1  Status (uuid, fw, wifi, ...)
    P->>B: ABF2  UnbindRequest  (clear the old owner FIRST)
    P->>B: ABF2  SensorScanRequest then SensorBindRequest (uuid)
    P->>B: ABF2  WifiSetRequest (ssid, password)
    P->>B: ABF2  SetTMPCertificateRequest (url, caPem, certPem, privateKey)  (LAST)
```
Codec + command list: [../03-reverse-engineering/ble-protocol.md](../03-reverse-engineering/ble-protocol.md).

## Transports & the internal "vsocket"
Inside the bridge, one framing layer (**vsocket**) is multiplexed by a *pipe id* and dispatched by a
*protocol* number:

| pipe (transport) | protocol space | reached over |
|---|---|---|
| 1 | BLE JSON (TEA) | BLE ABF2 |
| 0 | proto 2 — management / OTA-from-cloud | MQTT / BLE pipe 0 |
| 2 | proto 254 — plaintext config | **UART0 console** |
| (LoRa) | proto 0 — reader commands | 868 MHz radio |

This is why the same OTA and command handlers appear on multiple links. Reference:
[../03-reverse-engineering/firmware-layout.md](../03-reverse-engineering/firmware-layout.md).
