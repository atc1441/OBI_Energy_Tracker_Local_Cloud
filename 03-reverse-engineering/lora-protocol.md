# LoRa protocol (reader ↔ bridge, 868 MHz)

The reader (BAT32G135) and bridge (ESP32-C3) exchange framed messages over LoRa. This is **protocol 0**
of the bridge's internal dispatch.

## Frame format
```
Byte 0..2 : handle (3 bytes, PLAINTEXT)   ← per-sensor id; also the XOR key source
Byte 3    : [type:2 bits][cmd:6 bits]      ┐ XOR-"encrypted"
Byte 4..  : payload                         ┘ key = (b0 + b1 + b2) & 0xFF
```
- `type = byte3 >> 6`, `cmd = byte3 & 0x3F`.
- XOR applies from offset 3 to the end; the key is just the byte-sum of the cleartext handle → anyone
  who captures a frame can compute it. This is obfuscation, not encryption.
- `type` 1/2/3 select the bound-sensor data handlers (cmd 1/2/3); `type 0` uses the 6-bit `cmd`.

## Command set (Sensor → Bridge, RX)
| cmd | meaning | needs binding? |
|---|---|---|
| 1 / 2 / 3 | bound-sensor power/energy data (via `type`) | yes |
| 17 | **scan announce** — `[16B UUID][RSSI]` | no |
| 18 | offline reconnect | yes |
| 19 | energy data (16-bit power) | yes |
| 22 | energy "plus" data (32-bit power) | yes |
| 23 | energy realtime data | yes |
| 24 / 25 | energy data with trailing CRC16 | yes |
| 20 / 21 | **OTA block request** | **no** |
| 32 | **ECDH key exchange** — `[64B P-256 pubkey]` | no |

Bridge → Sensor (TX): scan request, bind, OTA responses, acks.

## Energy payload (cmd 19 / 22 / 23 / 24 / 25)
```
[0]      softver   (u8)
[1]      hardver   (u8)
[2]      battery   (u8, scaled → float in logs)
[3]      flags: bit0=infrared, bit1=lowpower, bit2=timesync, bit3=?
[4..7]   pos_power (u32, big-endian)
[8..11]  neg_power (u32, big-endian)
[12..13] power (u16)  ← cmd 19        | [12..15] power (u32) ← cmd 22/23/24/25
[14/16]  time_diff (u8, optional)
[17..18] crc16 (CRC-16/MODBUS)        ← cmd 24/25 only
```
`cmd19`=stored, `cmd22`=plus, `cmd23`=realtime. The bridge replies with a 3- or 8-byte ACK.

## Bound-sensor data (cmd 1/2/3, `diffpower`)
```
Format A (7B): [0..2] power (signed 22-bit) [3..4] energy (u16) [5..6] neg_energy (u16)
Format B (2B): [0..1] power (signed 14-bit)
```

## ECDH — cmd 32 (active gate, unused secret)
The reader sends a 64-byte P-256 public key; the bridge replies with its own 64-byte key, computes a
32-byte shared secret, and sets `key_ready`. **`key_ready` gates energy-data acceptance** (no exchange →
data rejected), so the handshake is *required*. However the shared secret itself is **never read** —
frames stay 1-byte XOR. Net: a liveness handshake with no cryptographic effect.

## OTA over LoRa (bridge = server, reader pulls)
The reader firmware lives **inside the bridge image** (marked `filec`, in the running app partition) and
is served read-only:
```
reader → bridge  cmd 21, 6-byte request: [.][.][u32 block-offset]
bridge → reader  if offset == 0xFFFFFFFF : frame cmd 12 = metadata (version, size, crc)
                 else                     : read 64B block + crc16 → frame cmd 71 (cmd 20 uses 69)
```
Because cmd 21 needs no binding and the XOR key is public, the served reader image can be pulled
block-by-block by any nearby radio — relevant if you want to extract or replace the reader firmware
(see [../05-lora-direct-868mhz](../05-lora-direct-868mhz/)).

## Radio settings
LoRa on SX1262, 868 MHz. SF/BW/CR/preamble are configured at runtime (not constants). BW index 0 =
125 kHz default. Capture the exact values by sniffing the SPI `0x86`/`0x8B` commands at boot — see
[../02-hardware/README.md](../02-hardware/README.md).
