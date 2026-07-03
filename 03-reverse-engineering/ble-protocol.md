# BLE protocol (phone ↔ bridge)

The bridge is a GATT **server**. Payloads are UTF-8 JSON, fragmented, then encrypted with **TEA**.

## GATT profile
| UUID | Type | Direction | Role |
|---|---|---|---|
| `0000ABF0-…-9b34fb` | Primary Service | – | container |
| `0000ABF1-…-9b34fb` | **Notify** (+ CCCD `2902`) | Bridge → App | **RX** (responses) |
| `0000ABF2-…-9b34fb` | **Write** | App → Bridge | **TX** (requests) |

Advertised name is `OBI-XXXXXX` (the 6-char suffix is the per-device "challenge id"). Local MTU 500;
the app requests MTU 180.

## Three layers
**A. JSON payload.** Request types (TX): `StatusRequest`, `WifiScanRequest`, `WifiSetRequest`
(`{ssid,password}`), `SensorScanRequest`, `SensorBindRequest` (`{uuid}`), `SensorRequest`,
`SetTMPCertificateRequest` (`{data:{url, provisioningTemplateName, caPem, certPem, privateKey}}`),
`UnbindRequest`. Responses (RX) carry their data under `data`, e.g.
`{"type":"WifiSet","data":{"ssid","connected","errorCode","errorDescription"}}`.

Command id mapping (firmware `ble_json_type_to_cmd`): Status=1, WifiScan=2, WifiSet=3, SensorScan=4,
SensorBind=5, Sensor=6, SetTMPCertificate=7, Unbind=8. Error codes for WifiSet: `0=OK`, `1=SSID Not
Exist`, `2=Connect Failed`; SetTMPCertificate: `0=OK`, `1=Failed to retrieve persistent certificate`.

> **SensorScan / SensorBind** (adding a reader) have their own walkthrough — response shapes, timing and
> the current reversing gaps — in [../07-add-a-reader/README.md](../07-add-a-reader/README.md).

**B. Fragmentation.** JSON bytes are split into ≤173-byte fragments; each frame:
```
Offset 0 : byte0  — bit7 = LAST flag, bits0..6 = packet number (0..126, rolling)
Offset 1 : index  — fragment order in the message
Offset 2 : len    — payload length of this fragment (≤173)
Offset 3.. : payload
+pad       : 0x00 until total length % 8 == 0
```
RX reassembles by packet number, sorts by index, concatenates when the LAST-flagged frame arrives.

**C. TEA (the whole frame).** Classic **TEA** (not XTEA):
```
block = 64 bit (2×32-bit little-endian) · key = 128 bit · 32 rounds · ECB (no IV/MAC)
sum starts 0, += 0x9E3779B9 each round     # firmware encodes it as sum -= 0x61C88647
v0 += ((v1<<4)+k0) ^ (v1+sum) ^ ((v1>>>5)+k1)
v1 += ((v0<<4)+k2) ^ (v0+sum) ^ ((v0>>>5)+k3)
```
Decrypt: `sum` starts `0xC6EF3720`, subtract per round. Reference implementation + CLI:
[../06-tools/obi_ble_codec.py](../06-tools/obi_ble_codec.py).

## The TEA key
16 bytes, **one per bridge**. Not derived on-device — it is **provisioned** and stored in NVS
(key `tea_key`). Two ways an owner can obtain it (see main [README](../README.md)):
1. authorized account via the app's challenge endpoint, or
2. straight off UART0 with the config protocol ([uart-config-protocol.md](uart-config-protocol.md)).

Example only (placeholder): `00112233445566778899AABBCCDDEEFF`.

## Round trip (example)
```
request  : {"type":"StatusRequest"}
frame    : 80 00 18 7b2274797065...7d 00.. (LAST|nr0, idx0, len0x18, JSON, 0-pad %8)
on wire  : TEA_ECB(frame, key)      # write to ABF2, one frame per write
response : TEA_ECB⁻¹(ABF1 notify) → defragment → {"type":"Status","data":{…}}
```
