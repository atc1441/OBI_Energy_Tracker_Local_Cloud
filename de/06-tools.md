# 06 · Tools (🇩🇪)

Eigenständige Helfer unter [`../06-tools/`](../06-tools/). Keine echten Keys — jeder Beispielwert ist ein
Platzhalter.

| Tool | Zweck |
|---|---|
| [`obi_uart_config.html`](../06-tools/obi_uart_config.html) | **Web‑Serial** UART‑Config‑Konsole: TEA‑Key & WLAN über das `C5 5C`‑Protokoll lesen/schreiben |
| [`obi_gateway_ble.html`](../06-tools/obi_gateway_ble.html) | **Web‑Bluetooth**‑Gateway: WLAN + Zertifikate (TEA+JSON) ans Gerät pushen |
| [`obi_ble_codec.py`](../06-tools/obi_ble_codec.py) | TEA‑Frame‑Codec: JSON ⇄ Fragmente ⇄ TEA (BLE‑Traffic en-/dekodieren) |

## obi_uart_config.html
In **Chrome oder Edge** öffnen (Web Serial braucht einen Chromium‑Browser; läuft auch von `file://`). Mit
der UART0 verbinden (115200 8N1). Ein‑Klick‑Reads für cmd 49 (TEA‑Key + IDs), 55 (WLAN‑Status), 59
(WLAN‑Daten); Builder für cmd 48/52/58. Berechnet CRC‑16/MODBUS, zeigt den Frame byteweise und dekodiert
Antworten (inkl. der aus dem gemischten Log‑Stream reassemblierten `C5 5C`‑Frames). Protokoll:
[03-uart-config.md](03-uart-config.md).

## obi_gateway_ble.html
In Chrome/Edge öffnen (Web Bluetooth). Scannt `OBI-XXXXXX`, verbindet Service `ABF0`, sendet
TEA‑verschlüsseltes JSON an `ABF2` (Status / WifiSet / SetTMPCertificate / Unbind). **Du musst den TEA‑Key
des Geräts eintragen** — das Feld enthält einen Platzhalter (`0011…EEFF`), keinen echten Key. Browser‑
Alternative zu `ble_provision.py`.

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

> Web Serial / Web Bluetooth brauchen einen Secure Context — Chromium‑Browser am Desktop; beides läuft von
> lokalem `file://`. Falls eine eingebettete Vorschau den Port‑Zugriff blockt, die Datei herunterladen und
> lokal öffnen.
