# Firmware‑Layout & Interna (🇩🇪)

## ESP32‑C3‑Gateway‑Image
Standard‑ESP‑IDF‑App‑Image (Magic `0xE9`, Chip‑ID 5 = ESP32‑C3), 6 Segmente:

| Seg | Load‑Adresse | Region |
|---|---|---|
| DROM | `0x3C120020` | const / rodata (Strings) |
| DRAM | `0x3FC93000` | .data |
| IRAM | `0x40380000`, `0x403880A0` | Code |
| IROM | `0x42000020` | Haupt‑`.text` (~1.2 MB) |
| RTC | `0x50000010` | rtc |

**Hinter den Segmenten (nach der Image‑SHA‑256) angehängt: die Reader‑Firmware** — ein rohes ARM‑Cortex‑M0+‑
Image (BAT32G135). Das ist der `filec`‑markierte Blob, den das Gateway per LoRa‑OTA an die Reader
ausliefert. Extraktion + IDA‑Laden: [../firmware/README.md](../firmware/README.md).

Zum Laden des Gateways selbst in IDA/Ghidra muss jedes Segment an seine Load‑Adresse gemappt werden (ein
flaches Laden dekompiliert zu Müll). Am einfachsten: die Segmente in eine RISC‑V‑ELF verpacken (ein PT_LOAD
pro Segment).

## Interner Dispatch (vsocket)
Eine Framing‑Schicht, gemultiplext über eine **Pipe‑ID**, verteilt über ein **Protokoll** via einer
`(proto<<16)|cmd`‑Handler‑Tabelle (`knock` registriert, `cmd_dispatch` ruft auf):

| Pipe | gefüttert von | Protokoll | Commands |
|---|---|---|---|
| 1 | BLE ABF2 | 1 | BLE‑JSON (Status/WifiSet/…/Unbind = 1..8) |
| 0 | MQTT / BLE Pipe0 | 2 | Management + **OTA‑aus‑Cloud** (file‑info=7, data=8) |
| 2 | **UART0** | 254 | Klartext‑Config (48/49/52/55/58/59) |
| (Funk) | LoRa | 0 | Reader‑Commands (17/19/22/…/32) |

Die Pipe‑ID ist durch den Transport fix — d. h. der UART‑Config‑Space (254) ist nicht über BLE erreichbar,
und BLE landet immer im TEA‑geschützten JSON‑Space (1).

## OTA — zwei Mechanismen
1. **Gateway‑Selbst‑Update (ESP32‑C3)** über MQTT (Proto 2): ein 23‑Byte‑`firmware-update-request`‑Offer
   `[3B Version][u32 total_len BE][16B md5]` → `esp_ota_begin` auf der *nächsten* OTA‑Partition; das Gerät
   zieht dann `firmware-data-request {offset}` und bekommt `firmware-data-response`‑Chunks
   `[3B Version][u32 Offset BE][u16 Len BE][data≤512]` → `esp_ota_write`; bei 100 % → Boot‑Partition setzen +
   Reboot. Pull‑basiert; Topics `…/ota/firmware-update-*` und `…/ota/firmware-data-*`. **Unsigniert** (nur
   md5) — volles Protokoll + Push‑Tool in [03-cloud-api.md#ota](03-cloud-api.md#ota). Live verifiziert
   (1.0.1 → 1.2.1).
2. **Reader‑Firmware‑Relay (BAT32G135)** über LoRa: unter 1.0.x liegt das Reader‑Image in der App‑Partition
   des Gateways und wird read‑only serviert (LoRa cmd 20/21). Unter **1.2.x** **holt das Gateway das
   Reader‑Image stattdessen aus der Cloud** über denselben `firmware-data-request`‑Kanal (separater
   `offset`‑Strom) und flasht den Reader dann über LoRa (`upgradeserver … upgrade success`; beobachtet Reader
   `32.0.0 → 57.0.0`).

## Boot‑Hardening (gesperrter Bootloader)
Beim Boot läuft ein Hardening‑Schritt (bedingt darauf, dass der USER_DATA‑eFuse gesetzt ist):
- brennt **DIS_JTAG**,
- brennt **DIS_DOWNLOAD_MODE**.

Beides einweg. Auf einem provisionierten Gerät heißt das: **kein `esptool`‑Flash‑Read/Write über UART** und
kein JTAG. Folge für Custom‑Firmware: direktes Flashen unmöglich — ein modifiziertes Image muss über den
**Cloud‑OTA‑Weg** (Mechanismus 1) kommen; deshalb ist die eigene Cloud der Weg zu Custom‑Firmware. Keine
Flash‑Encryption‑Strings im App‑Image.

## MQTT‑Topics (Struktur, Platzhalter für IDs)
```
cmd|dt /energy-tracking/bridge/<UUID>/sensor/<UUID>/state/live[-ack]
cmd|dt /energy-tracking/bridge/<UUID>/sensor/<UUID>/upload-interval-change-*
cmd|dt /energy-tracking/bridge/<UUID>/ota/firmware-update-*  ·  firmware-data-*
$aws/certificates/create/json[/accepted]
$aws/provisioning-templates/<template>/provision/json[/accepted]
$aws/rules/EnergyTrackingBridge[Heartbeat]/<UUID>/state
$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state
```
