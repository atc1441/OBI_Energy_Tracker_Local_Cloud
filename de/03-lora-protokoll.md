# LoRa‑Protokoll (Reader ↔ Gateway, 868 MHz) (🇩🇪)

Reader (BAT32G135) und Gateway (ESP32‑C3) tauschen geframte Nachrichten per LoRa. Das ist **Protokoll 0**
des internen Dispatch.

## Frame‑Format
```
Byte 0..2 : handle (3 Byte, KLARTEXT)     ← Sensor-ID; zugleich Schlüsselquelle
Byte 3    : [typ:2 Bit][cmd:6 Bit]         ┐ XOR-"verschlüsselt"
Byte 4..  : payload                         ┘ Key = (b0 + b1 + b2) & 0xFF
```
- `typ = byte3 >> 6`, `cmd = byte3 & 0x3F`.
- XOR ab Offset 3; der Key ist nur die Byte‑Summe des Klartext‑Handles → wer einen Frame mitschneidet, kann
  ihn berechnen. Obfuskation, keine Verschlüsselung.
- `typ` 1/2/3 wählen die Bound‑Sensor‑Handler (cmd 1/2/3); `typ 0` nutzt den 6‑Bit‑`cmd`.

## Timing — beacon‑synchronisiert (wer sendet wann)
Der Link ist ein **beacon‑synchronisierter Stern**: das **Gateway ist Master**, Reader/Outlets sind
zeit‑synchronisierte Slaves. Auf dem Gateway wartet `lora_beacon_task` auf einen **1000‑ms**‑Timer und ruft
`lora_beacon_tick` — also **einmal pro Sekunde** sendet das Gateway einen Beacon (das ist das stetige
`lora tx done` ~1 s im UART‑Log):

- **Zeit‑Sync‑Beacon** — `lora_send_time_beacon`: ein **Broadcast** (Handle `0xFFFFFB`) LoRa **cmd 15** mit dem
  aktuellen **Timestamp** des Gateways. Jedes Gerät hört ihn und synchronisiert seine Uhr auf das Gateway.
- **Interval‑Beacon** — `lora_send_interval_beacon`: passt das `upload_interval` eines gepaarten Geräts nicht
  zum Ziel (z. B. gerade per MQTT geändert), schickt das Gateway stattdessen einen **adressierten** Beacon
  (LoRa cmd 14), um das neue Intervall zu pushen. Reader können auch RSSI‑basiert eine Änderung anfordern
  (`dealBeaconFrame`).

Die **Geräte senden im Beacon‑Takt**: ein Reader/Outlet sendet seinen Energie/Leistungs‑Frame **alle
`upload_interval` Sekunden** (Standard 300, per MQTT setzbar — siehe
[03-cloud-api.md](03-cloud-api.md#downlink-kommandos-cloud--gerät)), zeit‑ausgerichtet auf den Beacon, plus
ein **Announce** in Scan‑Fenstern und die **ECDH**‑Frames beim Join. Also: Gateway → Beacon jede Sekunde
(Broadcast‑Zeit‑Sync, gelegentlich per‑Device‑Intervall‑Push); Gerät → Daten alle `upload_interval` s
(auf den Beacon ausgerichtet) + Scan‑Announces + Join‑Handshake. Nach jedem TX schaltet das Gateway den
SX1262 auf RX, um die Antworten zu empfangen (`lora_radio_tx_done_isr` → RX).

**Reader‑Seite (BAT32G135, reversed aus `reader_v1.2.1`).** Der Reader ist ein batteriebetriebener Slave, der
meist schläft. Er hält eine beacon‑synchronisierte Uhr `g_synced_time`, gesetzt aus dem Gateway‑Timestamp im
**cmd 15**‑Zeit‑Beacon (`reader_beacon_time_rx`) — der **cmd 14**‑Interval‑Beacon (`reader_beacon_interval_rx`)
pusht sowohl ein neues `upload_interval` *als auch* eine Uhr‑Resynchronisierung. Eine Timing‑State‑Machine
(`reader_lora_timing_sm`, Zustände 0/250/268/276) plant das nächste **aktive Fenster** des Readers auf
`g_synced_time + ~9,95 s` — er wacht also grob im **10‑Sekunden‑Takt** auf, um den 1‑Hz‑Beacon zu fangen und
synchron zu bleiben, hört ~100 ms; verpasst er den Beacon, versucht er es mit **+1 s Backoff (×10)** erneut und
resettet dann zum Neu‑Einrasten. Sein eigentlicher Uplink (optische Zähler‑Auslesung via
`reader_meter_read_iec62056` → `reader_obis_parse` → TEA‑verschlüsselter LoRa‑Energie‑Frame, cmd 19/22/…) geht
alle `upload_interval` Sekunden raus — **auf dieser synchronisierten Uhr** — und das Gateway bestätigt mit
cmd 38/40 (`reader_energy_ack_rx`). Kurz: der 1‑Hz‑Beacon des Gateways ist die gemeinsame Zeitbasis; der Reader
verfolgt ihn im ~10‑s‑Wachzyklus und sendet seine Zählerdaten einmal pro `upload_interval`.

## Command‑Satz (Sensor → Gateway, RX)
| cmd | Bedeutung | Bindung nötig? |
|---|---|---|
| 1 / 2 / 3 | Leistungs-/Energiedaten gebundener Sensoren (über `typ`) | ja |
| 17 | **Scan-Announce** — `[16B UUID][RSSI]` | nein |
| 18 | Offline-Reconnect | ja |
| 19 | Energiedaten (16-Bit-Leistung) | ja |
| 22 | Energie-"Plus"-Daten (32-Bit-Leistung) | ja |
| 23 | Energie-Echtzeitdaten | ja |
| 24 / 25 | Energiedaten mit CRC16 | ja |
| 20 / 21 | **OTA-Block-Anfrage** | **nein** |
| 32 | **ECDH-Key-Exchange** — `[64B P-256 Pubkey]` | nein |

Gateway → Sensor (TX): Scan‑Request, Bind, OTA‑Antworten, Acks.

## Energie‑Payload (cmd 19 / 22 / 23 / 24 / 25)
```
[0]      softver   (u8)
[1]      hardver   (u8)
[2]      battery   (u8, skaliert → float)
[3]      flags: bit0=infrared, bit1=lowpower, bit2=timesync, bit3=?
[4..7]   pos_power (u32, big-endian)
[8..11]  neg_power (u32, big-endian)
[12..13] power (u16)  ← cmd 19        | [12..15] power (u32) ← cmd 22/23/24/25
[14/16]  time_diff (u8, optional)
[17..18] crc16 (CRC-16/MODBUS)        ← nur cmd 24/25
```
`cmd19`=gespeichert, `cmd22`=plus, `cmd23`=Echtzeit. Das Gateway antwortet mit 3‑ oder 8‑Byte‑ACK.

## Bound‑Sensor‑Daten (cmd 1/2/3, `diffpower`)
```
Format A (7B): [0..2] power (signed 22-bit) [3..4] energy (u16) [5..6] neg_energy (u16)
Format B (2B): [0..1] power (signed 14-bit)
```

## Smart‑Outlet‑Commands (Firmware 1.2.x)
`1.2.x` ergänzt netzbetriebene **Smart‑Outlets** (Gerätetyp `0x11`) mit eigenem LoRa‑Command‑Bereich. Der
Outlet meldet Leistungsmessung und nimmt Relais-/Intervall‑Befehle vom Gateway (die es wiederum aus der Cloud
über `cmd/…/outlet/+/control` und `…/upload-interval-change-request` bekommt).

| cmd | Richtung | Bedeutung | Payload |
|---|---|---|---|
| 35 | Outlet → GW | Device‑Announce (beim Scan) | `[id][rssi]` |
| 41 | Outlet → GW | Outlet‑Daten | voltage(mV,u32) · current(mA,u32) · power(W,s32) · energy · negative_energy · relay |
| 43 | Outlet → GW | Outlet‑Daten (live) | dito |
| **45** | GW → Outlet | **Relais‑Steuerung** (an/aus) | `[u16=0][u8 relay]` — `lora_send_outlet_relay` |
| 46 | Outlet → GW | Relais‑Ack | — |
| **47** | GW → Outlet | **Upload‑Intervall‑Steuerung** | `[u16=0][u16 interval]` — `lora_send_outlet_interval` |
| 48 | Outlet → GW | Intervall‑Ack | — |

Cloud → Outlet: MQTT `outlet/<UUID>/control` → Proto2 cmd 10 (`mqtt_outlet_control_v12`) → **LoRa cmd 45**;
MQTT `outlet/<UUID>/upload-interval-change-request` → Proto2 cmd 11 → **LoRa cmd 47**. Die Outlet‑Telemetrie
wird als `EnergyTrackingOutlet/…/state` published — Schema in
[03-cloud-api.md](03-cloud-api.md#firmware-12x-telemetrie-reversed-schema_version-2). Auf 1.2.x sind diese
Frames **TEA‑verschlüsselt** (siehe ECDH‑Sektion).

## ECDH — cmd 32 (ungenutztes Secret auf 1.0.x; **der eigentliche LoRa‑Key auf 1.2.x**)
Der Reader sendet einen 64‑Byte‑P‑256‑Public‑Key; das Gateway antwortet mit seinem eigenen 64‑Byte‑Key,
berechnet ein 32‑Byte‑Shared‑Secret und setzt `key_ready`. **`key_ready` gated die Annahme von
Energiedaten** (kein Exchange → Daten verworfen), der Handshake ist also immer *Pflicht*.

- **1.0.x / 3x.x:** das Secret wird **nie gelesen** — Frames bleiben 1‑Byte‑XOR. Reines Liveness‑Gate ohne
  kryptografischen Effekt.
- **1.2.x:** das Secret **ist der LoRa‑Key**. `lora_cmd32_key_exchange` speichert es per Gerät (Struct `+143`,
  32 B) und jeder Frame‑Payload wird damit **TEA‑ECB** ver-/entschlüsselt (`lora_encrypt_frame` /
  `lora_decrypt_frame`), gated durch `key_ready` (`+78`). Auf 1.2.x greift die 1‑Byte‑XOR‑Analyse also nicht
  mehr — der Link ist echt verschlüsselt (siehe
  [03-security.md](03-security.md#2-lora-link-ohne-echte-krypto--nur-auf-10x--3xx-in-12x-gefixt)).

## OTA über LoRa (Gateway = Server, Reader zieht)
Die Reader‑Firmware liegt **im Gateway‑Image** (Markierung `filec`, in der laufenden App‑Partition) und wird
read‑only serviert:
```
Reader → Gateway  cmd 21, 6-Byte-Anfrage: [.][.][u32 Block-Offset]
Gateway → Reader  wenn Offset == 0xFFFFFFFF : Frame cmd 12 = Metadaten (Version, Größe, CRC)
                  sonst                     : 64B-Block lesen + crc16 → Frame cmd 71 (cmd 20 nutzt 69)
```
Da cmd 21 keine Bindung braucht und der XOR‑Key öffentlich ist, kann das gespeicherte Reader‑Image von jedem
nahen Funkgerät Block für Block gezogen werden — relevant, wenn du die Reader‑FW extrahieren/ersetzen willst
(siehe [05-lora-direkt.md](05-lora-direkt.md)).

## Radio‑Einstellungen (reversed aus `reader_v1.2.1` — exakte Werte)
Der Reader nutzt die **Semtech‑SX126x‑HAL** (`RadioInit`/`RadioSetTxConfig`/`RadioSetRxConfig`,
`sx126x_write_command` Opcodes `0x86`/`0x8B`/`0x8C`). `radio_init_config` setzt **einen festen Kanal** und
identische TX/RX‑Modulation — direkt aus den Init‑Argumenten dekodiert:

| Parameter | Wert | Quelle |
|---|---|---|
| **Frequenz** | **869,500 MHz** | `RadioSetChannel(869500000)` — Literal `0x33D38460` @ `.text:0xBF2C` |
| **Band** | EU868 SRD, 869,4–869,65 MHz Hochleistungs‑Sub‑Band (500 mW ERP, 10 % Duty‑Cycle) | — |
| **Modem** | LoRa | `modem = 1` |
| **Bandbreite** | **500 kHz** | `bw index = 2` → `g_lora_bw_table[2] = 0x06`; bestätigt durch den SX1262‑`reg 0x0889 &= 0xFB` **500‑kHz‑TX‑Modulation‑Errata‑Fix** |
| **Spreading Factor** | **SF7** | `datarate = 7` |
| **Coding Rate** | **4/5** | `coderate = 1` |
| **Preamble** | **12 Symbole** | `preambleLen = 12` |
| **Header** | **explizit** (variable Länge) | `fixLen = 0` |
| **CRC** | **an** | `crcOn = 1` |
| **IQ** | **normal** (nicht invertiert) | `iqInverted = 0` |
| **Sync‑Word** | **0x1424** (privat; SX127x‑Äquivalent `0x12`) | SX126x‑Reset‑Default — Firmware schreibt `REG_LR_SYNCWORD 0x0740` nie |
| **Sendeleistung** | **+22 dBm** | `power = 22`, `sx126x_set_tx_params` |

Reader und Gateway nutzen dieselbe Config (der Link ist symmetrisch) — ein drittes, identisch konfiguriertes
Funkgerät empfängt also jedes Reader↔Gateway‑Paket über die Luft.

### Mit eigenem ESP32 + SX1262 mitlesen (RadioLib)
Das reicht, um einen passiven Sniffer / eigenen Reader zu bauen. RadioLib auf ESP32‑C3/S3 mit SX1262‑Modul:
```cpp
#include <RadioLib.h>
SX1262 radio = new Module(NSS, DIO1, RST, BUSY);   // an dein Modul verdrahten
void setup() {
  // freq  bw     sf cr syncword           power  preamble
  radio.begin(869.5, 500.0, 7, 5, 0x12,    22,    12);
  radio.setCRC(true);        // expliziter Header + CRC an
  radio.setDio2AsRfSwitch(); // die meisten Module routen den RF‑Switch auf DIO2
  // RadioLib 0x12 => SX126x-Register 0x1424 (privates Sync-Word)
  radio.startReceive();
}
```
Beim RX bekommst du die rohe LoRa‑Payload = das [Frame‑Format](#frame-format) oben: 3‑Byte‑Klartext‑Handle,
dann `type|cmd`, dann die Payload. Auf **1.0.x/3x.x** ist die Payload 1‑Byte‑XOR (Key = Handle‑Byte‑Summe,
trivial rekonstruierbar). Auf **1.2.x** ist sie **TEA‑ECB** mit dem per‑Device‑ECDH‑Key — siehe
[ECDH‑Sektion](#ecdh--cmd-32-ungenutztes-secret-auf-10x-der-eigentliche-lora-key-auf-12x); ein passiver
Sniffer sieht Ciphertext + Klartext‑Handle/‑cmd, braucht aber den Key (aus dem Join oder per Gerät extrahiert),
um die Energiefelder zu lesen. Siehe [05-lora-direkt.md](05-lora-direkt.md) für das Empfänger‑Projekt.
