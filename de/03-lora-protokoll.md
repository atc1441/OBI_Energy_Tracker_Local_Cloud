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

## ECDH — cmd 32 (aktives Gate, ungenutztes Secret)
Der Reader sendet einen 64‑Byte‑P‑256‑Public‑Key; das Gateway antwortet mit seinem eigenen 64‑Byte‑Key,
berechnet ein 32‑Byte‑Shared‑Secret und setzt `key_ready`. **`key_ready` gated die Annahme von
Energiedaten** (kein Exchange → Daten verworfen), der Handshake ist also *Pflicht*. Das Secret selbst wird
jedoch **nie gelesen** — Frames bleiben 1‑Byte‑XOR. Fazit: ein Liveness‑Handshake ohne krypto­grafischen
Effekt.

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

## Radio‑Einstellungen
LoRa auf SX1262, 868 MHz. SF/BW/CR/Preamble werden zur Laufzeit gesetzt (keine Konstanten). BW‑Index 0 =
125 kHz (Default). Exakte Werte per SPI‑Sniff der Kommandos `0x86`/`0x8B` beim Boot — siehe
[02-hardware.md](02-hardware.md).
