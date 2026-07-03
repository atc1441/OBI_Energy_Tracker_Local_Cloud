# 05 · LoRa direct (868 MHz) — talk to the reader without the bridge

If you only want the meter data, you can skip the cloud and the bridge entirely and speak the reader's
LoRa protocol yourself with any SX126x/SX127x radio (e.g. a cheap SX1262 dev board or a LoRa-capable
SDR).

> Region-regulated RF. Operate within your local 868 MHz ISM rules (duty cycle etc.) and only with your
> own reader.

## What you need
- An SX126x radio on 868 MHz. Match the reader's LoRa PHY (SF/BW/CR/preamble). These are set at runtime
  in the bridge, so the reliable way to learn them is to **sniff the bridge's SPI at boot** and decode
  the SX1262 `0x86` (SetRfFrequency) and `0x8B` (SetModulationParams) commands — see
  [../02-hardware/README.md](../02-hardware/README.md). Start from **125 kHz BW** (the firmware default).
- The [LoRa frame format](../03-reverse-engineering/lora-protocol.md).

## Receiving the reader
The reader periodically transmits energy frames. To decode a captured frame:
```
1. strip the 3-byte handle (bytes 0..2, plaintext)
2. key = (b0 + b1 + b2) & 0xFF
3. XOR bytes 3..end with key
4. byte3 -> type = >>6, cmd = &0x3F
5. parse payload by cmd (energy layout for 19/22/23; see lora-protocol.md)
```
So the "encryption" is trivially reversible from the frame itself — you can read the meter values
(softver, hardver, battery, pos/neg power, OBIS-derived power) straight from the air.

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
