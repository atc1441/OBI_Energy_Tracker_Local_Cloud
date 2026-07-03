<a name="english"></a>
# Open OBI Energy Meter — ESP32 + SX1262 LoRa gateway

A **standalone firmware** for any ESP32 + Semtech SX1262 that fully **replaces the OBI/heyOBI bridge**.
It speaks the reader's proprietary LoRa protocol directly (pairing, ECDH, TEA decryption, energy decode),
serves a **web dashboard + MQTT**, lets you **set each reader's upload interval**, and can **flash reader
firmware over the air** — including rescuing a reader that is stuck in its bootloader. No vendor bridge, no
vendor cloud.

**🌐 Language:** **English** (below) · **[Deutsch ↓](#deutsch)**

> ⚠️ **Use on your own devices only.** Region-regulated RF — operate within your local 868 MHz ISM rules
> (duty cycle etc.). No vendor firmware binaries are shipped here; reader images you flash must come from a
> device you own (see [`../firmware/`](../firmware/)).

---

## What it does

The OBI system is a star network: a mains-powered **bridge** is the master, and battery **readers**
(BAT32G135, sitting on your electricity meter's optical port) are clients that wake up on a beacon-synced
schedule and report energy. This firmware **is that master.** It is not LoRaWAN — the modules are LoRa PHY
only, the protocol on top is a proprietary OBI protocol (own framing, command dispatch, XOR + TEA crypto,
bind + ECDH). Everything was reverse-engineered from firmware `1.2.1`; details in
[`../03-reverse-engineering/lora-protocol.md`](../03-reverse-engineering/lora-protocol.md).

## Features

| # | Feature | Notes |
|---|---|---|
| 1 | **Full bridge replacement** | 1 Hz time beacon, scan, announce/reconnect, bind, mutual **ECDH (P-256)** key exchange |
| 2 | **Both reader generations** | `1.2.x` (TEA-ECB with per-device ECDH key) **and** legacy `3x.x` (single-byte XOR) auto-detected |
| 3 | **Energy decode** | import / export / power, battery, firmware/hardware version, infrared & low-power flags |
| 4 | **Set upload interval** | per reader, from the web UI **or** MQTT — rides the encrypted energy-ACK the reader already waits for |
| 5 | **Reader firmware OTA over LoRa** | flash your own reader image; all three pull protocols served; can **un-brick** a reader stuck in its bootloader |
| 6 | **Web dashboard** | WiFi captive-portal setup, live reader cards, interval + firmware controls, DE/EN toggle |
| 7 | **MQTT** | publishes each reader as JSON; configurable in the UI; **command topic** to set the interval remotely |
| 8 | **Persistence** | reader UUIDs + MQTT settings saved in NVS, survive reboots |
| 9 | **Board-generic** | any ESP32 / ESP32-S3 + SX1262 via `board_config.h`; not tied to one vendor board |

## Hardware & wiring

Any ESP32 (classic or S3) plus a **Semtech SX1262** module. You need seven GPIOs plus the TCXO voltage.
Presets live in [`include/board_config.h`](include/board_config.h); pick one with a build flag.

| Signal | Heltec Vision Master E290 / V3 (`OBI_BOARD_HELTEC_S3`) | LILYGO T-Beam / T3 (`OBI_BOARD_TTGO_TBEAM_SX1262`) |
|---|---|---|
| NSS  | 8  | 18 |
| SCK  | 9  | 5  |
| MOSI | 10 | 27 |
| MISO | 11 | 19 |
| RST  | 12 | 23 |
| BUSY | 13 | 32 |
| DIO1 | 14 | 33 |
| TCXO | DIO3 @ 1.8 V | DIO3 @ 1.8 V |
| RF switch | DIO2 | DIO2 |

For any other wiring, edit the `OBI_BOARD_CUSTOM` block (set `LORA_TCXO_V 0.0f` if your module has a plain
crystal, not a TCXO).

### Radio parameters (reversed from `reader_v1.2.1`, exact)

| Frequency | Modem | BW | SF | CR | Preamble | Header | CRC | IQ | Sync word | TX power |
|---|---|---|---|---|---|---|---|---|---|---|
| **869.500 MHz** | LoRa | **500 kHz** | **7** | **4/5** | **12** | explicit | on | normal | **0x1424** (private / `0x12`) | +22 dBm |

## Quick start

```bash
# 1. Install PlatformIO (once)
pip install platformio

# 2. Build + flash for your board (from this folder)
pio run -e vision_master_e290 -t upload      # Heltec Vision Master E290 (ESP32-S3 + SX1262)
pio run -e ttgo_tbeam_sx1262  -t upload      # LILYGO T-Beam / T3
pio run -e generic_esp32s3    -t upload      # your own wiring (edit board_config.h → OBI_BOARD_CUSTOM)
pio run -e generic_esp32      -t upload      # classic ESP32 + your own wiring

# 3. Watch the serial log
pio device monitor -b 115200
```

**First boot:** the device opens a WiFi setup portal named **`OBI-Gateway-Setup`** (`192.168.4.1`). Join it,
enter your WiFi (and optionally MQTT), save. The dashboard is then at the IP printed on the serial log
(`http://<device-ip>/`). LoRa runs immediately — turn the vendor bridge **off** and the readers will
re-pair to this gateway on their own within a minute.

## Web dashboard

- **WiFi captive portal** (WiFiManager) — no hard-coded credentials; reconfigurable any time.
- **Live reader cards**: 3-byte id, 16-byte **UUID**, device type, firmware/hardware version, RSSI,
  battery, infrared (meter-reading) status, and import / export / power.
- **Per reader**: set the **upload interval** and **flash firmware** (`.bin` picker → over-the-air update).
- **Bootloader badge**: a reader that reset into its OTA bootloader is shown with a ⚙ *"Bootloader mode —
  ready to flash"* badge so you can (re)arm firmware for it even though it isn't sending energy.
- **MQTT panel** (⚙): configure host/port/user/pass/base-topic and see live status (connected, last
  publish, message count).
- **DE/EN toggle**, remembered in the browser.
- The web/MQTT stack runs on **core 0** while LoRa runs on **core 1**, so HTTP traffic never stutters the
  radio timing.

## Setting the upload interval

Two ways, same effect (the value is delivered inside the TEA-encrypted energy-ACK that the reader already
waits ~300 ms for after each report):

- **Web UI** — type seconds into a reader card and press *Interval*.
- **MQTT** — publish the seconds as the payload to `‹base›/‹id›/set_interval`:

  ```bash
  mosquitto_pub -h 192.168.1.91 -u USER -P PASS \
    -t obi/gateway/238d4e/set_interval -m 30
  ```
  `‹base›` is your configured base topic (default `obi/gateway`), `‹id›` the 6-hex reader id. The gateway
  logs `mqtt rx set_interval 238d4e -> 30 s` and applies it on the reader's next report.

## MQTT integration

- **Publish**: every reader is published as JSON to `‹base›/‹id›` (e.g. `obi/gateway/d70c9a`):
  ```json
  {"id":"d70c9a","uuid":"…","type":"meter","battery_mV":3080,"rssi":-74,
   "infrared":true,"import":83049758,"export":875840,"power":null}
  ```
- **Subscribe**: `‹base›/+/set_interval` (see above).
- **Status** is exposed on `/api/status` (`connected`, `state`, `pub_count`, `last_pub_s`) and shown in the
  dashboard MQTT panel.
- All settings are configurable in the web UI and persisted in NVS.

## Reader firmware OTA over LoRa

Pick a reader's `.bin`, press **Flash firmware**, confirm the warning. The gateway advertises the new
version in the energy-ACK; after three acks the reader resets into its **bootloader** and pulls the image
block-by-block, which the gateway serves. The reader validates the image (CRC) **before** writing, so a
bad file is rejected, not bricked.

Three pull protocols are implemented and verified — the gateway answers whichever the reader uses:

| Reader request | Gateway response | Block layout |
|---|---|---|
| cmd 33 (newer) | cmd 34 | `[type][offset:4][ver][64 B][crc16]` (crc after block) |
| cmd 21 (bootloader) | cmd 53 | `[offset:4][f1][crc16][64 B]` (crc before block) |
| cmd 20 | cmd 52 | `[offset:4][ver][64 B]` (no crc) |

A reader that is **already stuck in its bootloader** (begging with cmd 20/21/33) is picked up
automatically: it appears on the dashboard in bootloader mode, so you can arm an image and recover it.
This was verified live — a reader that would not boot was pulled to a known-good version and came back
online. Reader images must be dumped from a device you own; see [`../firmware/`](../firmware/).

## Architecture & source files

The firmware is split so the radio state machine and the network stack never block each other.

| File | Responsibility |
|---|---|
| [`src/main.cpp`](src/main.cpp) | LoRa state machine: RX dispatch, beacon/scan/bind TX, ECDH, energy decode + ACK, OTA serving |
| [`include/obi_proto.h`](include/obi_proto.h) | Frame build/parse, `obi_crc16` (MODBUS split-table), `obi_xor`, TEA-ECB encrypt/decrypt (little-endian words) |
| [`include/obi_ecdh.h`](include/obi_ecdh.h) · [`src/obi_ecdh.cpp`](src/obi_ecdh.cpp) | mbedTLS secp256r1 key-pair + shared secret (TEA key = first 16 bytes of shared X) |
| [`include/reader.h`](include/reader.h) | Shared `Reader` state (identity, key, telemetry, interval, bootloader flag) + the web↔LoRa API |
| [`include/gateway_web.h`](include/gateway_web.h) · [`src/gateway_web.cpp`](src/gateway_web.cpp) | WiFiManager portal, HTTP dashboard, REST API, MQTT client (publish + command RX) — pinned to core 0 |
| [`include/board_config.h`](include/board_config.h) | Pin presets per board |
| [`platformio.ini`](platformio.ini) | Build envs + library deps (RadioLib, WiFiManager, PubSubClient) |

**Key functions in `main.cpp`:** `sendBeacon` (cmd 15), `sendScan` (36), `sendBind` (59),
`sendEcdhReply` (32), `sendEnergyAck` (38/40, TEA-encrypted, carries interval + version), `serveOtaRequest`
(cmd 33→34), `serveOtaLegacy` (cmd 20/21→52/53), `handleRx` (the command dispatch), and the
`gw_ota_*` / `gw_request_interval` bridge that the web UI calls.

**How pairing works** (all handled automatically): the reader is passive and waits for the gateway. It
accepts our 1 Hz beacon (it only checks the gateway id), announces (cmd 17/35) or reconnects (cmd 18/58);
we scan-ack + bind (cmd 59); the reader runs a mutual **ECDH P-256** exchange (cmd 32) and from then on
TEA-encrypts its energy payloads with the derived key. Legacy `3x.x` readers do the same dance with the
older cmd 49/50 acks and an un-encrypted payload layout. Turn the vendor bridge off and it happens on its
own — no factory reset required.

## Notes & limits

- **Reader button:** a *short* press activates the infrared/optical readout (values start flowing,
  `infrared` flips to 1); a **~10-second** hold factory-resets/re-pairs the reader.
- Smart-outlet devices (type `0x11`, cmd 41/43) are identified in the protocol but not decoded here — the
  dashboard targets meter readers.
- OTA is unsigned on the analyzed firmware (integrity CRC only), which is what makes custom reader firmware
  possible; re-verify on your own unit, firmware can change.

## Reference

- LoRa frame + commands: [`../03-reverse-engineering/lora-protocol.md`](../03-reverse-engineering/lora-protocol.md)
- Talking to the reader passively (sniffer): [`../05-lora-direct-868mhz/`](../05-lora-direct-868mhz/)
- Reader = BAT32G135, OBIS / IEC-62056: [`../02-hardware/`](../02-hardware/)
- Reader firmware in IDA (bring your own dump): [`../firmware/`](../firmware/)

---
<a name="deutsch"></a>
# Open OBI Energy Meter — ESP32 + SX1262 LoRa-Gateway  🇩🇪

Eine **eigenständige Firmware** für beliebige ESP32 + Semtech SX1262, die die **OBI/heyOBI-Bridge komplett
ersetzt**. Sie spricht direkt das proprietäre LoRa-Protokoll der Reader (Kopplung, ECDH, TEA-Entschlüsselung,
Energie-Dekodierung), bietet ein **Web-Dashboard + MQTT**, lässt das **Upload-Intervall je Reader** einstellen
und kann **Reader-Firmware über die Luft flashen** — inklusive Rettung eines Readers, der in seinem Bootloader
festhängt. Keine Hersteller-Bridge, keine Hersteller-Cloud.

**🌐 Sprache:** [English ↑](#english) · **Deutsch** (unten)

> ⚠️ **Nur an eigenen Geräten verwenden.** Regulierte Funkfrequenzen — halte die lokalen 868-MHz-ISM-Regeln
> (Duty-Cycle usw.) ein. Es werden keine Hersteller-Firmware-Binaries mitgeliefert; Reader-Images müssen von
> einem eigenen Gerät stammen (siehe [`../firmware/`](../firmware/)).

---

## Was es macht

Das OBI-System ist ein Sternnetz: eine netzbetriebene **Bridge** ist der Master, und batteriebetriebene
**Reader** (BAT32G135, am optischen Port deines Stromzählers) sind Clients, die beacon-synchron aufwachen und
Energie melden. Diese Firmware **ist dieser Master.** Es ist kein LoRaWAN — die Module sind nur LoRa-PHY, das
Protokoll darüber ist ein proprietäres OBI-Protokoll (eigenes Framing, Command-Dispatch, XOR- + TEA-Krypto,
Bind + ECDH). Alles wurde aus Firmware `1.2.1` reversed; Details in
[`../03-reverse-engineering/lora-protocol.md`](../03-reverse-engineering/lora-protocol.md).

## Funktionen

| # | Funktion | Hinweise |
|---|---|---|
| 1 | **Vollständiger Bridge-Ersatz** | 1-Hz-Zeit-Beacon, Scan, Announce/Reconnect, Bind, gegenseitiger **ECDH (P-256)**-Schlüsseltausch |
| 2 | **Beide Reader-Generationen** | `1.2.x` (TEA-ECB mit Pro-Gerät-ECDH-Schlüssel) **und** Legacy `3x.x` (1-Byte-XOR), automatisch erkannt |
| 3 | **Energie-Dekodierung** | Bezug / Einspeisung / Leistung, Batterie, Firmware-/Hardware-Version, Infrarot- & Low-Power-Flags |
| 4 | **Upload-Intervall setzen** | je Reader, über die Web-UI **oder** MQTT — reist im verschlüsselten Energie-ACK mit, auf den der Reader ohnehin wartet |
| 5 | **Reader-Firmware-OTA über LoRa** | eigenes Reader-Image flashen; alle drei Pull-Protokolle bedient; kann einen im Bootloader festhängenden Reader **wiederbeleben** |
| 6 | **Web-Dashboard** | WLAN-Einrichtung per Captive-Portal, Live-Reader-Karten, Intervall- + Firmware-Steuerung, DE/EN-Umschalter |
| 7 | **MQTT** | publiziert jeden Reader als JSON; in der UI konfigurierbar; **Command-Topic** zum Fern-Setzen des Intervalls |
| 8 | **Persistenz** | Reader-UUIDs + MQTT-Einstellungen im NVS gespeichert, überstehen Neustarts |
| 9 | **Board-generisch** | beliebiger ESP32 / ESP32-S3 + SX1262 via `board_config.h`; nicht an ein Hersteller-Board gebunden |

## Hardware & Verdrahtung

Beliebiger ESP32 (klassisch oder S3) plus ein **Semtech SX1262**-Modul. Du brauchst sieben GPIOs plus die
TCXO-Spannung. Presets liegen in [`include/board_config.h`](include/board_config.h); per Build-Flag wählen.

| Signal | Heltec Vision Master E290 / V3 (`OBI_BOARD_HELTEC_S3`) | LILYGO T-Beam / T3 (`OBI_BOARD_TTGO_TBEAM_SX1262`) |
|---|---|---|
| NSS  | 8  | 18 |
| SCK  | 9  | 5  |
| MOSI | 10 | 27 |
| MISO | 11 | 19 |
| RST  | 12 | 23 |
| BUSY | 13 | 32 |
| DIO1 | 14 | 33 |
| TCXO | DIO3 @ 1,8 V | DIO3 @ 1,8 V |
| RF-Schalter | DIO2 | DIO2 |

Für andere Verdrahtung den `OBI_BOARD_CUSTOM`-Block anpassen (`LORA_TCXO_V 0.0f`, falls dein Modul einen
Quarz statt eines TCXO hat).

### Funkparameter (aus `reader_v1.2.1` reversed, exakt)

| Frequenz | Modem | BW | SF | CR | Präambel | Header | CRC | IQ | Sync-Wort | Sendeleistung |
|---|---|---|---|---|---|---|---|---|---|---|
| **869,500 MHz** | LoRa | **500 kHz** | **7** | **4/5** | **12** | explizit | an | normal | **0x1424** (privat / `0x12`) | +22 dBm |

## Schnellstart

```bash
# 1. PlatformIO installieren (einmalig)
pip install platformio

# 2. Für dein Board bauen + flashen (aus diesem Ordner)
pio run -e vision_master_e290 -t upload      # Heltec Vision Master E290 (ESP32-S3 + SX1262)
pio run -e ttgo_tbeam_sx1262  -t upload      # LILYGO T-Beam / T3
pio run -e generic_esp32s3    -t upload      # eigene Verdrahtung (board_config.h → OBI_BOARD_CUSTOM)
pio run -e generic_esp32      -t upload      # klassischer ESP32 + eigene Verdrahtung

# 3. Serielles Log ansehen
pio device monitor -b 115200
```

**Erster Start:** das Gerät öffnet ein WLAN-Einrichtungsportal namens **`OBI-Gateway-Setup`**
(`192.168.4.1`). Verbinde dich damit, gib dein WLAN (und optional MQTT) ein, speichern. Das Dashboard ist
dann unter der im seriellen Log ausgegebenen IP erreichbar (`http://‹geräte-ip›/`). LoRa läuft sofort —
schalte die Hersteller-Bridge **aus**, und die Reader koppeln sich innerhalb einer Minute von selbst an
dieses Gateway.

## Web-Dashboard

- **WLAN-Captive-Portal** (WiFiManager) — keine fest verdrahteten Zugangsdaten; jederzeit neu konfigurierbar.
- **Live-Reader-Karten**: 3-Byte-ID, 16-Byte-**UUID**, Gerätetyp, Firmware-/Hardware-Version, RSSI,
  Batterie, Infrarot-(Zähler-Lese-)Status sowie Bezug / Einspeisung / Leistung.
- **Je Reader**: **Upload-Intervall** setzen und **Firmware flashen** (`.bin`-Auswahl → Over-the-Air-Update).
- **Bootloader-Badge**: ein Reader, der in seinen OTA-Bootloader neu gestartet ist, wird mit einem
  ⚙ *"Bootloader-Modus — bereit zum Flashen"* angezeigt, damit du auch dann Firmware für ihn scharfschalten
  kannst, wenn er keine Energie sendet.
- **MQTT-Panel** (⚙): Host/Port/Benutzer/Passwort/Basis-Topic konfigurieren und Live-Status sehen
  (verbunden, zuletzt gesendet, Anzahl).
- **DE/EN-Umschalter**, im Browser gemerkt.
- Der Web-/MQTT-Stack läuft auf **Core 0**, LoRa auf **Core 1**, damit HTTP-Verkehr das Funk-Timing nie stört.

## Upload-Intervall setzen

Zwei Wege, gleiche Wirkung (der Wert wird im TEA-verschlüsselten Energie-ACK ausgeliefert, auf den der
Reader nach jedem Bericht ohnehin ~300 ms wartet):

- **Web-UI** — Sekunden in eine Reader-Karte eintippen und *Intervall* drücken.
- **MQTT** — die Sekunden als Payload an `‹base›/‹id›/set_interval` publizieren:

  ```bash
  mosquitto_pub -h 192.168.1.91 -u USER -P PASS \
    -t obi/gateway/238d4e/set_interval -m 30
  ```
  `‹base›` ist dein konfiguriertes Basis-Topic (Standard `obi/gateway`), `‹id›` die 6-stellige Hex-Reader-ID.
  Das Gateway loggt `mqtt rx set_interval 238d4e -> 30 s` und wendet es beim nächsten Bericht des Readers an.

## MQTT-Anbindung

- **Publish**: jeder Reader wird als JSON an `‹base›/‹id›` publiziert (z. B. `obi/gateway/d70c9a`):
  ```json
  {"id":"d70c9a","uuid":"…","type":"meter","battery_mV":3080,"rssi":-74,
   "infrared":true,"import":83049758,"export":875840,"power":null}
  ```
- **Subscribe**: `‹base›/+/set_interval` (siehe oben).
- **Status** liegt auf `/api/status` (`connected`, `state`, `pub_count`, `last_pub_s`) und wird im
  MQTT-Panel angezeigt.
- Alle Einstellungen sind in der Web-UI konfigurierbar und im NVS persistiert.

## Reader-Firmware-OTA über LoRa

Reader-`.bin` auswählen, **Firmware flashen** drücken, Warnung bestätigen. Das Gateway kündigt die neue
Version im Energie-ACK an; nach drei ACKs startet der Reader in seinen **Bootloader** und zieht das Image
blockweise, das das Gateway ausliefert. Der Reader validiert das Image (CRC) **vor** dem Schreiben — eine
kaputte Datei wird abgelehnt, nicht gebrickt.

Drei Pull-Protokolle sind implementiert und verifiziert — das Gateway antwortet auf das, was der Reader nutzt:

| Reader-Anfrage | Gateway-Antwort | Block-Layout |
|---|---|---|
| cmd 33 (neuer) | cmd 34 | `[type][offset:4][ver][64 B][crc16]` (CRC nach Block) |
| cmd 21 (Bootloader) | cmd 53 | `[offset:4][f1][crc16][64 B]` (CRC vor Block) |
| cmd 20 | cmd 52 | `[offset:4][ver][64 B]` (kein CRC) |

Ein Reader, der **bereits im Bootloader festhängt** (bettelt mit cmd 20/21/33), wird automatisch erkannt:
er erscheint im Dashboard im Bootloader-Modus, sodass du ein Image scharfschalten und ihn retten kannst.
Live verifiziert — ein Reader, der nicht mehr startete, wurde auf eine funktionierende Version gezogen und
kam wieder online. Reader-Images müssen von einem eigenen Gerät stammen; siehe [`../firmware/`](../firmware/).

## Architektur & Quelldateien

Die Firmware ist so aufgeteilt, dass sich Funk-Statemachine und Netzwerk-Stack nie gegenseitig blockieren.

| Datei | Aufgabe |
|---|---|
| [`src/main.cpp`](src/main.cpp) | LoRa-Statemachine: RX-Dispatch, Beacon/Scan/Bind-TX, ECDH, Energie-Dekodierung + ACK, OTA-Auslieferung |
| [`include/obi_proto.h`](include/obi_proto.h) | Frame-Bau/-Parse, `obi_crc16` (MODBUS-Split-Table), `obi_xor`, TEA-ECB Ver-/Entschlüsseln (Little-Endian-Words) |
| [`include/obi_ecdh.h`](include/obi_ecdh.h) · [`src/obi_ecdh.cpp`](src/obi_ecdh.cpp) | mbedTLS secp256r1 Schlüsselpaar + Shared Secret (TEA-Schlüssel = erste 16 Byte von Shared X) |
| [`include/reader.h`](include/reader.h) | Gemeinsamer `Reader`-Zustand (Identität, Schlüssel, Telemetrie, Intervall, Bootloader-Flag) + die Web↔LoRa-API |
| [`include/gateway_web.h`](include/gateway_web.h) · [`src/gateway_web.cpp`](src/gateway_web.cpp) | WiFiManager-Portal, HTTP-Dashboard, REST-API, MQTT-Client (Publish + Command-RX) — auf Core 0 gepinnt |
| [`include/board_config.h`](include/board_config.h) | Pin-Presets je Board |
| [`platformio.ini`](platformio.ini) | Build-Envs + Bibliotheken (RadioLib, WiFiManager, PubSubClient) |

**Wichtige Funktionen in `main.cpp`:** `sendBeacon` (cmd 15), `sendScan` (36), `sendBind` (59),
`sendEcdhReply` (32), `sendEnergyAck` (38/40, TEA-verschlüsselt, trägt Intervall + Version), `serveOtaRequest`
(cmd 33→34), `serveOtaLegacy` (cmd 20/21→52/53), `handleRx` (der Command-Dispatch) sowie die
`gw_ota_*`- / `gw_request_interval`-Brücke, die die Web-UI aufruft.

**Wie die Kopplung läuft** (alles automatisch): der Reader ist passiv und wartet auf das Gateway. Er
akzeptiert unser 1-Hz-Beacon (prüft nur die Gateway-ID), meldet sich an (cmd 17/35) oder verbindet neu
(cmd 18/58); wir scan-acken + binden (cmd 59); der Reader führt einen gegenseitigen **ECDH-P-256**-Tausch
durch (cmd 32) und verschlüsselt ab dann seine Energie-Payloads per TEA mit dem abgeleiteten Schlüssel.
Legacy-`3x.x`-Reader machen dasselbe mit den älteren cmd-49/50-ACKs und einem unverschlüsselten Payload-
Layout. Hersteller-Bridge ausschalten — der Rest passiert von selbst, kein Werksreset nötig.

## Hinweise & Grenzen

- **Reader-Taste:** ein *kurzer* Druck aktiviert die Infrarot-/optische Lesung (Werte fließen, `infrared`
  springt auf 1); ein **~10-sekündiges** Halten setzt den Reader zurück / koppelt ihn neu.
- Smart-Steckdosen (Typ `0x11`, cmd 41/43) sind im Protokoll identifiziert, hier aber nicht dekodiert — das
  Dashboard zielt auf Zähler-Reader.
- OTA ist auf der analysierten Firmware unsigniert (nur Integritäts-CRC), was eigene Reader-Firmware erst
  möglich macht; am eigenen Gerät gegenprüfen, Firmware kann sich ändern.

## Referenz

- LoRa-Frame + Kommandos: [`../03-reverse-engineering/lora-protocol.md`](../03-reverse-engineering/lora-protocol.md)
- Passiv mitlesen (Sniffer): [`../05-lora-direct-868mhz/`](../05-lora-direct-868mhz/)
- Reader = BAT32G135, OBIS / IEC-62056: [`../02-hardware/`](../02-hardware/)
- Reader-Firmware in IDA (eigener Dump): [`../firmware/`](../firmware/)
