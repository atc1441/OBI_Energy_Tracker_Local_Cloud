# 05 · LoRa direct (868 MHz) — talk to the reader without the bridge

If you only want the meter data, you can skip the cloud and the bridge entirely and speak the reader's
LoRa protocol yourself with any SX126x/SX127x radio (e.g. a cheap SX1262 dev board or a LoRa-capable
SDR).

> Region-regulated RF. Operate within your local 868 MHz ISM rules (duty cycle etc.) and only with your
> own reader.

> 💡 **Want the finished firmware, not just the protocol?** A complete, ready-to-flash **mini-gateway**
> (pairing + ECDH + energy decode, web dashboard, MQTT, set-interval, reader firmware OTA) lives in
> **[Open OBI Energy Meter](../open_obi_energy_meter/)**. This page explains the protocol *behind* it.

## What you need
- An SX126x radio (e.g. a cheap SX1262 dev board). The reader's LoRa PHY is now **fully reversed** from
  `reader_v1.2.1` — no SPI sniffing needed. It is a **single fixed channel**, TX and RX identical:

  | Frequency | Modem | BW | SF | CR | Preamble | Header | CRC | IQ | Sync word | TX power |
  |---|---|---|---|---|---|---|---|---|---|---|
  | **869.500 MHz** | LoRa | **500 kHz** | **7** | **4/5** | **12** | explicit | on | normal | **0x1424** (private / `0x12`) | +22 dBm |

  Full derivation + a RadioLib config snippet:
  [../03-reverse-engineering/lora-protocol.md · Radio settings](../03-reverse-engineering/lora-protocol.md#radio-settings-reversed-from-reader_v121--exact-values).
- The [LoRa frame format](../03-reverse-engineering/lora-protocol.md#frame-format).

## Building an ESP32 + SX1262 receiver
An ESP32-C3/S3 with an SX1262 module and RadioLib is enough to passively read the air:
```cpp
#include <RadioLib.h>
SX1262 radio = new Module(NSS, DIO1, RST, BUSY);
void setup() {
  radio.begin(869.5, 500.0, 7, 5, 0x12, 22, 12);  // freq bw sf cr sync power preamble
  radio.setCRC(true);
  radio.setDio2AsRfSwitch();
  radio.startReceive();
}
// in loop(): radio.readData(buf, len) -> decode frame below
```

## Decoding a captured frame
```
1. strip the 3-byte handle (bytes 0..2, plaintext)
2. byte3 -> type = >>6, cmd = &0x3F      (do this on the DECRYPTED payload)
3a. firmware 1.0.x / 3x.x:  key = (b0+b1+b2) & 0xFF ; XOR bytes 3..end  -> trivially recoverable from the frame
3b. firmware 1.2.x:         payload is TEA-ECB with the per-device ECDH key (see below) — NOT recoverable passively
4. parse payload by cmd (energy layout for 19/22/23; see lora-protocol.md)
```
The plaintext handle + `type|cmd` are always visible, so you can always see **who** is talking and **which**
frame it is. On **1.0.x** the whole payload is readable straight from the air (the XOR key is the handle
byte-sum). On **1.2.x** the energy fields are genuinely **TEA-encrypted** — a passive sniffer sees ciphertext;
to read the meter values you need that device's key, either by **observing/answering the ECDH join** (you act
as the gateway, below) or by extracting it. See
[lora-protocol.md · ECDH](../03-reverse-engineering/lora-protocol.md#ecdh--cmd-32-unused-secret-on-10x-the-actual-lora-key-on-12x).

## Acting as the bridge (bidirectional)
To fully replace the bridge you implement the RX dispatch and the TX side:
- Answer **cmd 32** (ECDH): the reader won't send energy data until `key_ready` is set, so you must
  complete the key-exchange handshake (send a 64-byte P-256 public key back). The shared secret is not
  used afterwards, so the exchange just needs to *complete*.
- Handle **cmd 17** (announce) and issue scan/bind so the reader considers itself paired.
- Optionally serve **OTA** (cmd 20/21) if you want to push your own reader firmware — the reader pulls
  64-byte blocks by offset; you supply metadata (frame cmd 12) then blocks (frame cmd 71). Source the
  blocks from a reader image you dumped yourself — see [../firmware/README.md](../firmware/README.md)
  (no vendor binaries are shipped here).

## Pulling the reader firmware over the air
Because cmd 21 needs no binding, you can also request the **bridge's** stored reader image block-by-block
(request `[.][.][u32 offset]`, receive 64-byte blocks) — handy to dump the exact image a given bridge
would flash to its readers.

## Reference material
- Frame + commands: [../03-reverse-engineering/lora-protocol.md](../03-reverse-engineering/lora-protocol.md)
- Reader firmware in IDA (bring your own dump): [../firmware/README.md](../firmware/README.md)
- Reader = BAT32G135, OBIS/IEC-62056 meter reader: [../02-hardware/README.md](../02-hardware/README.md)
