# BLE‑Protokoll (App ↔ Gateway) (🇩🇪)

Das Gateway ist GATT‑**Server**. Payloads sind UTF‑8‑JSON, fragmentiert, dann mit **TEA** verschlüsselt.

## GATT‑Profil
| UUID | Typ | Richtung | Rolle |
|---|---|---|---|
| `0000ABF0-…-9b34fb` | Primary Service | – | Container |
| `0000ABF1-…-9b34fb` | **Notify** (+ CCCD `2902`) | Gateway → App | **RX** (Antworten) |
| `0000ABF2-…-9b34fb` | **Write** | App → Gateway | **TX** (Requests) |

Advertising‑Name ist `OBI-XXXXXX` (die 6‑Zeichen‑Endung ist die geräte­spezifische „Challenge‑ID"). Lokale
MTU 500; die App fordert MTU 180.

## Drei Schichten
**A. JSON‑Payload.** Request‑Typen (TX): `StatusRequest`, `WifiScanRequest`, `WifiSetRequest`
(`{ssid,password}`), `SensorScanRequest`, `SensorBindRequest` (`{uuid}`), `SensorRequest`,
`SetTMPCertificateRequest` (`{data:{url, provisioningTemplateName, caPem, certPem, privateKey}}`),
`UnbindRequest`. Antworten (RX) tragen ihre Nutzlast unter `data`, z. B.
`{"type":"WifiSet","data":{"ssid","connected","errorCode","errorDescription"}}`.

Command‑ID‑Mapping (`ble_type_to_cmd`): Status=1, WifiScan=2, WifiSet=3, SensorScan=4, SensorBind=5,
Sensor=6, SetTMPCertificate=7, Unbind=8. **Firmware 1.2.x ergänzt** DevicesScan=9, DevicesBind=10,
DevicesUnbind=11, DevicesRequest=12, FactoryReset=13, BluetoothDisable=14 (die `Devices*`‑Familie ist die
generische Multi‑Device-/Smart‑Outlet‑Variante von `Sensor*`). Fehlercodes WifiSet: `0=OK`, `1=SSID Not
Exist`, `2=Connect Failed`; SetTMPCertificate: `0=OK`, `1=Failed to retrieve persistent certificate`.

**Status‑Response (1.2.x, `ble_cmd_status`):**
```json
{ "type": "Status", "data": { "uuid": "<bridge>", "firmware_version": "1.2.1", "hardware_version": "6.0.0",
    "connected_wifi": "<ssid>" | null, "wifi_set": false, "persistent_cert_set": false,
    "ble_protocol_version": "2.2" } }
```
`connected_wifi` ist die verbundene SSID (oder `null`); `ble_protocol_version` ist nur ein Versions‑String —
Framing/Krypto sind unverändert (Einzel‑Fragment‑Nachrichten funktionieren problemlos).

**WifiScan‑Response (`ble_wifi_scan_response`)** — async; das Gerät scannt und antwortet dann:
```json
{ "type": "WifiScan", "data": { "wifi_list": [ { "ssid": "ATClan", "rssi": -45 },
    { "ssid": "gigacube-…", "rssi": -88 } ] } }
```
Nur `ssid` + `rssi` pro AP (kein BSSID im BLE‑JSON).

> **SensorScan / SensorBind** (Reader hinzufügen) haben eine eigene Anleitung — Antwortformate, Timing und
> die offenen Reversing‑Lücken — in [07-reader-koppeln.md](07-reader-koppeln.md).

**B. Fragmentierung.** JSON‑Bytes in ≤173‑Byte‑Fragmente; je Frame:
```
Offset 0 : Byte0  — Bit7 = LAST-Flag, Bits0..6 = Paketnummer (0..126, rollierend)
Offset 1 : index  — Reihenfolge in der Nachricht
Offset 2 : len    — Payload-Länge dieses Fragments (≤173)
Offset 3.. : Payload
+Pad       : 0x00 bis Gesamtlänge % 8 == 0
```
RX gruppiert nach Paketnummer, sortiert nach index, setzt bei LAST‑Flag zusammen.

**C. TEA (der ganze Frame).** Klassisches **TEA** (nicht XTEA):
```
Block = 64 Bit (2×32-Bit little-endian) · Key = 128 Bit · 32 Runden · ECB (kein IV/MAC)
sum startet 0, += 0x9E3779B9 je Runde     # Firmware kodiert es als sum -= 0x61C88647
v0 += ((v1<<4)+k0) ^ (v1+sum) ^ ((v1>>>5)+k1)
v1 += ((v0<<4)+k2) ^ (v0+sum) ^ ((v0>>>5)+k3)
```
Decrypt: `sum` startet `0xC6EF3720`, pro Runde subtrahieren. Referenz‑Implementierung + CLI:
[../06-tools/obi_ble_codec.py](../06-tools/obi_ble_codec.py).

## Der TEA‑Key
16 Byte, **einer pro Gateway**. Nicht am Gerät berechnet — **provisioniert** und in NVS gespeichert (Key
`tea_key`). Zwei Wege ihn zu holen (siehe [../ANLEITUNG.md](../ANLEITUNG.md)):
1. autorisiertes Konto über den Challenge‑Endpoint der App, oder
2. direkt über UART0 mit dem Config‑Protokoll ([03-uart-config.md](03-uart-config.md)).

Nur Beispiel (Platzhalter): `00112233445566778899AABBCCDDEEFF`.
