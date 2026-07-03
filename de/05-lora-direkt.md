# 05 · LoRa direkt (868 MHz) — mit dem Reader ohne Gateway sprechen (🇩🇪)

Wenn du nur die Zählerdaten willst, kannst du Cloud und Gateway ganz überspringen und das LoRa‑Protokoll des
Readers selbst mit einem SX126x/SX127x‑Radio sprechen (z. B. ein günstiges SX1262‑Board oder ein
LoRa‑fähiger SDR).

> Regulierter Funk. Nur innerhalb deiner lokalen 868‑MHz‑ISM‑Regeln (Duty‑Cycle etc.) und nur mit deinem
> eigenen Reader.

## Was du brauchst
- Ein SX126x‑Radio (z. B. ein günstiges SX1262‑Board). Die LoRa‑PHY des Readers ist jetzt **vollständig
  reversed** aus `reader_v1.2.1` — kein SPI‑Sniff nötig. Es ist **ein fester Kanal**, TX und RX identisch:

  | Frequenz | Modem | BW | SF | CR | Preamble | Header | CRC | IQ | Sync‑Word | Sendeleistung |
  |---|---|---|---|---|---|---|---|---|---|---|
  | **869,500 MHz** | LoRa | **500 kHz** | **7** | **4/5** | **12** | explizit | an | normal | **0x1424** (privat / `0x12`) | +22 dBm |

  Volle Herleitung + RadioLib‑Snippet:
  [03-lora-protokoll.md · Radio‑Einstellungen](03-lora-protokoll.md#radio-einstellungen-reversed-aus-reader_v121--exakte-werte).
- Das [LoRa‑Frame‑Format](03-lora-protokoll.md#frame-format).

## ESP32 + SX1262 Empfänger bauen
Ein ESP32‑C3/S3 mit SX1262‑Modul und RadioLib reicht, um passiv mitzulesen:
```cpp
#include <RadioLib.h>
SX1262 radio = new Module(NSS, DIO1, RST, BUSY);
void setup() {
  radio.begin(869.5, 500.0, 7, 5, 0x12, 22, 12);  // freq bw sf cr sync power preamble
  radio.setCRC(true);
  radio.setDio2AsRfSwitch();
  radio.startReceive();
}
// in loop(): radio.readData(buf, len) -> Frame unten dekodieren
```

## Einen erfassten Frame dekodieren
```
1. die 3-Byte-Handle abziehen (Byte 0..2, Klartext)
2. Byte3 -> typ = >>6, cmd = &0x3F        (auf der ENTSCHLÜSSELTEN Payload)
3a. Firmware 1.0.x / 3x.x:  Key = (b0+b1+b2) & 0xFF ; Bytes 3..Ende XORen  -> trivial aus dem Frame
3b. Firmware 1.2.x:         Payload ist TEA-ECB mit dem per-Device-ECDH-Key (siehe unten) — passiv NICHT lesbar
4. Payload je cmd parsen (Energie-Layout für 19/22/23; siehe 03-lora-protokoll.md)
```
Handle + `typ|cmd` sind immer im Klartext sichtbar — du siehst also stets **wer** sendet und **welcher** Frame
es ist. Auf **1.0.x** ist die ganze Payload direkt aus der Luft lesbar (XOR‑Key = Handle‑Byte‑Summe). Auf
**1.2.x** sind die Energiefelder echt **TEA‑verschlüsselt** — ein passiver Sniffer sieht Ciphertext; um die
Zählerwerte zu lesen, brauchst du den Key des Geräts, entweder durch **Beobachten/Beantworten des ECDH‑Joins**
(du agierst als Gateway, unten) oder durch Extraktion. Siehe
[03-lora-protokoll.md · ECDH](03-lora-protokoll.md#ecdh--cmd-32-ungenutztes-secret-auf-10x-der-eigentliche-lora-key-auf-12x).

## Als Gateway agieren (bidirektional)
Um das Gateway voll zu ersetzen, implementierst du RX‑Dispatch und die TX‑Seite:
- **cmd 32** (ECDH) beantworten: der Reader sendet keine Energiedaten, bis `key_ready` gesetzt ist — der
  Handshake muss also *abgeschlossen* werden (sende einen 64‑Byte‑P‑256‑Pubkey zurück). Das Secret wird
  danach nicht genutzt, der Exchange muss nur durchlaufen.
- **cmd 17** (Announce) verarbeiten und Scan/Bind ausgeben, damit der Reader sich als gepaart betrachtet.
- Optional **OTA** (cmd 20/21) servieren, um eigene Reader‑Firmware zu pushen — der Reader zieht 64‑Byte‑
  Blöcke nach Offset; du lieferst Metadaten (Frame cmd 12) und dann Blöcke (Frame cmd 71). Blöcke nimmst du
  aus einem selbst gedumpten Reader‑Image — siehe [../firmware/README.md](../firmware/README.md) (keine
  Vendor‑Binaries im Repo).

## Reader‑Firmware über die Luft ziehen
Da cmd 21 keine Bindung braucht, kannst du auch das **im Gateway** gespeicherte Reader‑Image Block für Block
anfordern (Request `[.][.][u32 Offset]`, 64‑Byte‑Blöcke empfangen) — praktisch, um das exakte Image zu
dumpen, das ein Gateway an seine Reader flashen würde.

## Referenzen
- Frame + Commands: [03-lora-protokoll.md](03-lora-protokoll.md)
- Reader‑Firmware in IDA (eigenes Dump mitbringen): [../firmware/README.md](../firmware/README.md)
- Reader = BAT32G135, OBIS/IEC‑62056‑Zähler‑Reader: [02-hardware.md](02-hardware.md)
