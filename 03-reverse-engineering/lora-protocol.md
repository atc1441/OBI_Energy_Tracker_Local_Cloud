# LoRa protocol (reader ↔ bridge, 868 MHz)

The reader (BAT32G135) and bridge (ESP32-C3) exchange framed messages over LoRa. This is **protocol 0**
of the bridge's internal dispatch.

> **Not LoRaWAN.** The devices use **LoRa only as the radio/PHY** (Semtech SX126x modulation). Everything
> above it — framing, command dispatch, the beacon-synchronised timing, TEA/XOR crypto, bind + ECDH — is a
> **proprietary OBI protocol**, documented here from the firmware.

## Frame format
```
Byte 0..2 : handle (3 bytes, PLAINTEXT)   ← per-sensor id; also the XOR key source
Byte 3    : [type:2 bits][cmd:6 bits]      ┐ XOR-"encrypted"
Byte 4..  : payload                         ┘ key = (b0 + b1 + b2) & 0xFF
```
- `type = byte3 >> 6`, `cmd = byte3 & 0x3F`.
- XOR applies from offset 3 to the end; the key is just the byte-sum of the cleartext handle → anyone
  who captures a frame can compute it. This is obfuscation, not encryption.
- **This XOR is only the outer layer.** The **energy payload** underneath is additionally **TEA-ECB**
  encrypted with the per-device ECDH key — on the old v32 readers (cloud "1.0.1") *and* 1.2.x (see the
  [ECDH section](#ecdh--cmd-32-the-actual-lora-key-on-both-v32-and-12x)). Control frames stay plaintext
  under the XOR.
- `type` 1/2/3 select the bound-sensor data handlers (cmd 1/2/3); `type 0` uses the 6-bit `cmd`.

## Timing — beacon-synchronized (who sends when)
The link is a **beacon-synchronized star**: the **bridge is the master** and the readers/outlets are
time-synced slaves. On the bridge, `lora_beacon_task` waits on a **1000 ms** timer and calls
`lora_beacon_tick` — so **once per second** the bridge transmits a beacon (this is the steady `lora tx done`
you see every ~1 s in the UART log):

- **Time-sync beacon** — `lora_send_time_beacon`: a **broadcast** (handle `0xFFFFFB`) LoRa **cmd 15** carrying
  the bridge's current **timestamp**. Every device hears it and syncs its clock to the bridge.
- **Interval beacon** — `lora_send_interval_beacon`: if a paired device's `upload_interval` doesn't match the
  target (e.g. you just changed it over MQTT), the bridge instead sends an **addressed** beacon (LoRa cmd 14)
  to push the new interval to that device. Readers can also request a change based on RSSI (`dealBeaconFrame`).

The **devices transmit on the beacon clock**: a reader/outlet sends its energy/power frame **every
`upload_interval` seconds** (default 300, settable via MQTT — see [cloud-api.md](cloud-api.md#downlink-commands-cloud--device)),
time-aligned to the beacon, plus an **announce** during scan windows and the **ECDH** frames when it joins.
So: bridge → beacon every 1 s (broadcast time-sync, occasional per-device interval push); device → data every
`upload_interval` s (aligned to that beacon) + scan announces + join handshake. After each TX the bridge flips
the SX1262 to RX to catch the device replies (`lora_radio_tx_done_isr` → RX).

**Reader side (BAT32G135, reversed from `reader_v1.2.1`).** The reader is a battery-powered slave that is
mostly asleep. It keeps a beacon-synced clock `g_synced_time`, written from the gateway timestamp in the
**cmd 15** time beacon (`reader_beacon_time_rx`) — the **cmd 14** interval beacon (`reader_beacon_interval_rx`)
also both pushes a new `upload_interval` *and* re-syncs the clock. A timing state machine
(`reader_lora_timing_sm`, states 0/250/268/276) schedules the reader's next **active window** at
`g_synced_time + ~9.95 s` — i.e. it wakes on roughly a **10-second cadence** to catch the 1 Hz beacon and stay
synced, listening ~100 ms; if it misses the beacon it retries with a **+1 s backoff (×10)** then resets to
re-acquire. Its actual uplink (optical meter read via `reader_meter_read_iec62056` → `reader_obis_parse` →
TEA-encrypted LoRa energy frame, cmd 19/22/…) is sent every `upload_interval` seconds **on that synced clock**,
and the gateway ACKs with cmd 38/40 (`reader_energy_ack_rx`). Net: the gateway's 1 Hz beacon is the shared
time base; the reader tracks it on a ~10 s wake cycle and transmits its meter data once per `upload_interval`.

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

## Smart-outlet commands (firmware 1.2.x)
`1.2.x` adds mains **smart outlets** (device type `0x11`), which use the same LoRa link with their own
command range. The outlet reports power metering and takes relay/interval commands from the gateway (which
the gateway in turn receives from the cloud on `cmd/…/outlet/+/control` and `…/upload-interval-change-request`).

| cmd | dir | meaning | payload |
|---|---|---|---|
| 35 | outlet → gw | device announce (during scan) | `[id][rssi]` |
| 41 | outlet → gw | outlet data | voltage(mV,u32) · current(mA,u32) · power(W,s32) · energy · negative_energy · relay |
| 43 | outlet → gw | outlet data (live) | same |
| **45** | gw → outlet | **relay control** (on/off) | `[u16=0][u8 relay]` — from `lora_send_outlet_relay` |
| 46 | outlet → gw | relay-control ack | — |
| **47** | gw → outlet | **upload-interval control** | `[u16=0][u16 interval]` — from `lora_send_outlet_interval` |
| 48 | outlet → gw | interval-control ack | — |

Cloud → outlet path: MQTT `outlet/<UUID>/control` → proto2 cmd 10 (`mqtt_outlet_control_v12`) → **LoRa cmd 45**;
MQTT `outlet/<UUID>/upload-interval-change-request` → proto2 cmd 11 → **LoRa cmd 47**. The outlet telemetry
is published as `EnergyTrackingOutlet/…/state` — schema in
[cloud-api.md](cloud-api.md#firmware-12x-telemetry-reversed-schema_version-2). On 1.2.x these frames are
**TEA-encrypted** (see the ECDH section below).

## ECDH — cmd 32 (**the actual LoRa key on both v32 and 1.2.x**)
The reader sends a 64-byte P-256 public key; the bridge replies with its own 64-byte key, computes a
32-byte shared secret, and sets `key_ready`. **`key_ready` gates energy-data acceptance** (no exchange →
data rejected), so the handshake is always *required*.

The shared secret **is the LoRa key** — and (correcting an earlier assumption here) that is true on the
**old v32 readers too**, not just 1.2.x. This was confirmed by building the ESP32 gateway
([../open_obi_energy_meter](../open_obi_energy_meter)): the old reader whose cloud firmware string is
`1.0.1` reports **softver 32 ("v32")** on the link and **requires** its energy ACK (cmd 24→56 / 25→57) to
be **TEA-encrypted** with the ECDH-derived key (reversed from its ack handler `sub_B7A0`); its energy
uplink is decoded via the same TEA path (`tea/legacy`).

- **v32 (cloud "1.0.1") / 3x.x:** `lora_cmd32_key_exchange` result → **TEA-ECB** payload with the legacy
  energy layout and legacy command numbers (cmd 24/25 energy, 56/57 acks). Same crypto as 1.2.x, older
  frame layout.
- **1.2.x:** the shared secret is stored per device (struct `+143`, 32 B) and every frame payload is
  **TEA-ECB** encrypted/decrypted with it (`lora_encrypt_frame` / `lora_decrypt_frame`), gated by the
  `key_ready` flag (`+78`), with the newer CRC-prefixed energy layout.

Only the **outer frame XOR** (byte3..end, handle-sum key) is common to all versions and remains trivially
recoverable; the *energy payload* itself is TEA-encrypted on both generations (see
[security-notes.md](security-notes.md)).

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

## Radio settings (reversed from `reader_v1.2.1` — exact values)
The reader runs the **Semtech SX126x HAL** (`RadioInit`/`RadioSetTxConfig`/`RadioSetRxConfig`,
`sx126x_write_command` opcodes `0x86`/`0x8B`/`0x8C`). `radio_init_config` sets **one fixed channel** and
identical TX/RX modulation — decoded straight from the init call arguments:

| Parameter | Value | Source |
|---|---|---|
| **Frequency** | **869.500 MHz** | `RadioSetChannel(869500000)` — literal `0x33D38460` @ `.text:0xBF2C` |
| **Band** | EU868 SRD, 869.4–869.65 MHz high-power sub-band (500 mW ERP, 10 % duty cycle) | — |
| **Modem** | LoRa | `modem = 1` |
| **Bandwidth** | **500 kHz** | `bw index = 2` → `g_lora_bw_table[2] = 0x06`; confirmed by the SX1262 `reg 0x0889 &= 0xFB` **500 kHz TX-modulation errata fix** |
| **Spreading factor** | **SF7** | `datarate = 7` |
| **Coding rate** | **4/5** | `coderate = 1` |
| **Preamble** | **12 symbols** | `preambleLen = 12` |
| **Header** | **explicit** (variable length) | `fixLen = 0` |
| **CRC** | **on** | `crcOn = 1` |
| **IQ** | **normal** (not inverted) | `iqInverted = 0` |
| **Sync word** | **0x1424** (private; SX127x-equivalent `0x12`) | SX126x reset default — firmware never writes `REG_LR_SYNCWORD 0x0740` |
| **TX power** | **+22 dBm** | `power = 22`, `sx126x_set_tx_params` |

Both the reader and the gateway use this same config (the link is symmetric), so a third radio configured
identically can receive every reader↔gateway packet over the air.

### Reading it with your own ESP32 + SX1262 (RadioLib)
This is enough to build a passive sniffer / own reader. RadioLib on ESP32-C3/S3 with an SX1262 module:
```cpp
#include <RadioLib.h>
SX1262 radio = new Module(NSS, DIO1, RST, BUSY);   // wire to your module
void setup() {
  // freq  bw     sf cr syncword           power  preamble
  radio.begin(869.5, 500.0, 7, 5, 0x12,    22,    12);
  radio.setCRC(true);        // explicit header + CRC on
  radio.setDio2AsRfSwitch(); // most modules route the RF switch on DIO2
  // RadioLib's 0x12 => SX126x register 0x1424 (private sync word)
  radio.startReceive();
}
```
On RX you get the raw LoRa payload = the [frame format](#frame-format) above: 3-byte plaintext handle, then
`type|cmd`, then the payload. The `byte3..end` **outer XOR** (key = handle byte-sum) is trivially
recoverable on every version and reveals `type/cmd` + control-frame payloads. The **energy payload**,
however, is **TEA-ECB** with the per-device ECDH key on **both** the old v32 readers **and** 1.2.x — see the
[ECDH section](#ecdh--cmd-32-the-actual-lora-key-on-both-v32-and-12x); a passive sniffer sees the
ciphertext and the plaintext handle/cmd, but needs the key (from the join, or extracted per device) to read
the energy fields. See [../05-lora-direct-868mhz](../05-lora-direct-868mhz/) for the receiver project.
