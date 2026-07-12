# UART‑Config‑Protokoll (`C5 5C`) — der Self‑Hosting‑Einstieg (🇩🇪)

Die UART0‑Konsole des Gateways (**115200 8N1**, GPIO20 RX / GPIO21 TX) trägt neben dem Log einen
**Klartext**‑Config‑Kanal. Der schnellste Weg, den TEA‑Key zu lesen und WLAN umzustellen, wenn dir das
Gateway physisch gehört. Über BLE **nicht** erreichbar (der Transport fixiert die Pipe‑ID).

## Frame‑Format
```
C5 5C | LEN(2, big-endian) | CRC(2) | FE | CMD | PAYLOAD…
```
- `LEN` = Gesamt‑Byte‑Zahl des Frames (ab `C5`).
- `CRC` = **CRC‑16/MODBUS** (Init `0xFFFF`) über alles ab `FE`; `00 00` **überspringt** die Prüfung. Auf dem
  Draht: die zwei CRC‑Bytes als `[lo][hi]`.
- `FE` = Protokoll‑Marker (254). `CMD` = eines der sechs unten.
- Antwort: gleiches Framing, mit `CMD | 0x80` und dem Handler‑Blob als Payload.

## Commands
| cmd | Rtg | Request‑Payload | Response‑Payload |
|---|---|---|---|
| **48** `0x30` | schreiben | 38 B: `UUID(16) + BLE-ID(6) + TEA-Key(16)` | Ack |
| **49** `0x31` | lesen | — | 39 B: `marker(1) + UUID(16) + BLE-ID(6) + TEA-Key(16)` |
| **52** `0x34` | schreiben | `[N][N Bytes]` → sendet einen LoRa-Test-Frame | Ack |
| **55** `0x37` | lesen | — | WLAN-Status + SSID |
| **58** `0x3A` | schreiben | `[ssid_len][pwd_len][ssid][passwort]` | Ack |
| **59** `0x3B` | lesen | — | `[status][ssid_len][pwd_len][SSID][PASSWORT]` |

> ⚠️ **Stolperstein Stock‑Firmware 1.0.1:** `pwd_len` ist ein einzelnes Byte (Wire‑Kapazität bis 255), aber
> die Stock‑Gateway‑Firmware selbst verarbeitet WLAN‑Passwörter nur bis **32 Byte** korrekt — ein längeres
> Passwort wird stillschweigend abgeschnitten bzw. abgelehnt, und das Gerät verbindet sich nicht. Bei einer
> Stock‑(Nicht‑Custom‑)Firmware das WLAN‑Passwort über cmd 58 auf ≤32 Zeichen halten.

## Fertige Frames
```
TEA-Key + IDs lesen (cmd 49):    C5 5C 00 08 00 00 FE 31         # CRC 0000 = skip
WLAN-Daten lesen (cmd 59):       C5 5C 00 08 00 00 FE 3B
TEA-Key setzen (cmd 48, 38B):    C5 5C 00 2E 00 00 FE 30 <16B UUID><6B BLE-ID><16B TEA-KEY>
```

## Browser‑Tool
[../06-tools/obi_uart_config.html](../06-tools/obi_uart_config.html) in Chrome/Edge öffnen (Web Serial):
Ein‑Klick‑Reads für cmd 49/55/59, Builder für 48/52/58, automatische CRC und Response‑Dekodierung.

> Auch (Boot‑Log): Die Firmware liest beim Start einen 16‑Byte‑eFuse‑USER_DATA‑Wert und gibt ihn im selben
> UART‑Log aus ("Key content"). Wer die Key‑Herkunft untersucht, schneidet das Boot‑Log mit.
