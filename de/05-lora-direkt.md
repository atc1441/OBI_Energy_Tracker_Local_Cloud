# 05 · LoRa direkt (868 MHz) — mit dem Reader ohne Gateway sprechen (🇩🇪)

Wenn du nur die Zählerdaten willst, kannst du Cloud und Gateway ganz überspringen und das LoRa‑Protokoll des
Readers selbst mit einem SX126x/SX127x‑Radio sprechen (z. B. ein günstiges SX1262‑Board oder ein
LoRa‑fähiger SDR).

> Regulierter Funk. Nur innerhalb deiner lokalen 868‑MHz‑ISM‑Regeln (Duty‑Cycle etc.) und nur mit deinem
> eigenen Reader.

## Was du brauchst
- Ein SX126x‑Radio auf 868 MHz. Passe die LoRa‑PHY des Readers (SF/BW/CR/Preamble) an. Die werden im Gateway
  zur Laufzeit gesetzt — sicher erfährst du sie per **SPI‑Sniff beim Boot** (Kommandos `0x86`/`0x8B`
  dekodieren), siehe [02-hardware.md](02-hardware.md). Starte mit **125 kHz BW** (Firmware‑Default).
- Das [LoRa‑Frame‑Format](03-lora-protokoll.md).

## Reader empfangen
Der Reader sendet periodisch Energie‑Frames. Einen erfassten Frame dekodieren:
```
1. die 3-Byte-Handle abziehen (Byte 0..2, Klartext)
2. Key = (b0 + b1 + b2) & 0xFF
3. Bytes 3..Ende mit Key XORen
4. Byte3 -> typ = >>6, cmd = &0x3F
5. Payload je cmd parsen (Energie-Layout für 19/22/23; siehe 03-lora-protokoll.md)
```
Die „Verschlüsselung" ist also aus dem Frame selbst trivial umkehrbar — du liest die Zählerwerte (Version,
Batterie, pos/neg Leistung, OBIS‑abgeleitete Leistung) direkt aus der Luft.

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
