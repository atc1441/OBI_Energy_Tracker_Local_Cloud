# 06 · Tools (🇩🇪)

Eigenständige Helfer unter [`../06-tools/`](../06-tools/). Keine echten Keys — jeder Beispielwert ist ein
Platzhalter.

| Tool | Zweck |
|---|---|
| [`obi_uart_config.html`](../06-tools/obi_uart_config.html) | **Web‑Serial** UART‑Config‑Konsole: TEA‑Key & WLAN über das `C5 5C`‑Protokoll lesen/schreiben |
| [`obi_gateway_ble.html`](../06-tools/obi_gateway_ble.html) | **Web‑Bluetooth**‑Gateway: WLAN + Zertifikate (TEA+JSON) ans Gerät pushen |
| [`obi_ble_codec.py`](../06-tools/obi_ble_codec.py) | TEA‑Frame‑Codec: JSON ⇄ Fragmente ⇄ TEA (BLE‑Traffic en-/dekodieren) |
| [`split_flash_dump.py`](../06-tools/split_flash_dump.py) | Einen Full‑Flash‑Dump (aus /debug „Dump full flash") in einzelne Partitions‑`.bin` zerlegen |

## obi_uart_config.html
In **Chrome oder Edge** öffnen (Web Serial braucht einen Chromium‑Browser; läuft auch von `file://`). Mit
der UART0 verbinden (115200 8N1). Ein‑Klick‑Reads für cmd 49 (TEA‑Key + IDs), 55 (WLAN‑Status), 59
(WLAN‑Daten); Builder für cmd 48/52/58. Berechnet CRC‑16/MODBUS, zeigt den Frame byteweise und dekodiert
Antworten (inkl. der aus dem gemischten Log‑Stream reassemblierten `C5 5C`‑Frames). Protokoll:
[03-uart-config.md](03-uart-config.md).

> ⚠️ Die SSID-/Passwort‑Felder von cmd 58 erlauben auf dem Draht bis zu 255 Byte je Feld, aber die
> **Stock‑Gateway‑Firmware 1.0.1 verarbeitet WLAN‑Passwörter nur bis 32 Byte** korrekt — ein längeres wird
> stillschweigend abgeschnitten bzw. abgelehnt, und das Gerät verbindet sich nicht. Bei einer
> Stock‑(Nicht‑Custom‑)Firmware das Passwort auf ≤32 Zeichen halten.

## obi_gateway_ble.html
In Chrome/Edge öffnen (Web Bluetooth). Scannt `OBI-XXXXXX`, verbindet Service `ABF0`, sendet
TEA‑verschlüsseltes JSON an `ABF2` (Status / WifiSet / SetTMPCertificate / Unbind). **Du musst den TEA‑Key
des Geräts eintragen** — das Feld enthält einen Platzhalter (`0011…EEFF`), keinen echten Key. Browser‑
Alternative zu `ble_provision.py`.

> ⚠️ Dasselbe **32‑Byte‑WLAN‑Passwort‑Limit** wie oben gilt auf Stock‑Firmware 1.0.1 auch für die
> WifiSet‑Felder hier. **Unter Linux** steckt Web Bluetooth in Chrome/Chromium hinter einem Flag — vorher
> `chrome://flags/#enable-experimental-web-platform-features` aktivieren und den Browser neu starten, sonst
> findet die Seite das Gerät nicht.

## obi_ble_codec.py
Reiner Stdlib‑Referenz‑Codec (dokumentiert in [03-ble-protokoll.md](03-ble-protokoll.md)):
```bash
# JSON -> verschlüsselte Frames (Hex, für ABF2)
python obi_ble_codec.py encode --key <32hex> --data '{"type":"StatusRequest"}'
# erfasste ABF1-Frames -> JSON
python obi_ble_codec.py decode --key <32hex> <framehex> [<framehex> ...]
# rohe TEA-ECB-Blockoperation
python obi_ble_codec.py tea-enc --key <32hex> --block 0011223344556677
```
Jeder Key, den du übergibst, ist dein eigener; die eingebauten Beispiel‑Keys sind Platzhalter.

## split_flash_dump.py
Reiner‑Stdlib‑Splitter für einen **Full‑Flash‑Dump**, den du über die `/debug`‑Seite der Custom‑Firmware
gezogen hast („Dump full flash" → `flash_full_*.bin`). Liest die ESP32‑Partitionstabelle bei `0x8000` aus
dem Dump und schreibt jede Partition in eine eigene Datei — so kommen die zwei App‑Slots (`ota_0`/`ota_1`,
die laufende und die vorherige Firmware) getrennt heraus.
```bash
python split_flash_dump.py flash_full_4096k.bin --list          # nur die Partitionstabelle zeigen
python split_flash_dump.py flash_full_4096k.bin -o out/         # jede Partition schreiben
python split_flash_dump.py flash_full_4096k.bin -o out/ --only app0,app1
```
Kompletter Walkthrough (Dumpen + Partitionen teilen): [04-flash-dumpen.md](04-flash-dumpen.md).

> Web Serial / Web Bluetooth brauchen einen Secure Context — Chromium‑Browser am Desktop; beides läuft von
> lokalem `file://`. Falls eine eingebettete Vorschau den Port‑Zugriff blockt, die Datei herunterladen und
> lokal öffnen.
