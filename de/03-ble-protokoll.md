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

Command‑ID‑Mapping (`ble_json_type_to_cmd`): Status=1, WifiScan=2, WifiSet=3, SensorScan=4, SensorBind=5,
Sensor=6, SetTMPCertificate=7, Unbind=8. Fehlercodes WifiSet: `0=OK`, `1=SSID Not Exist`, `2=Connect
Failed`; SetTMPCertificate: `0=OK`, `1=Failed to retrieve persistent certificate`.

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
