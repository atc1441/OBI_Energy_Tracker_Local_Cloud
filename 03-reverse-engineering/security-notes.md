# Security notes

Factual weaknesses found while reversing, so **owners can assess and self-host** their units. Ordered by
what matters for ownership, not by severity theatre.

## 1. TEA key readable/writable over UART (plaintext)
The UART0 config channel (`C5 5C …`) has **no authentication**. `cmd 49` returns the 16-byte TEA key,
`cmd 48` sets it, and `cmd 58/59` write/read WiFi credentials — all in the clear. Anyone with access to
the console pins (GPIO20/21) owns the BLE channel and the WiFi. This is also the intended self-host entry
point. See [uart-config-protocol.md](uart-config-protocol.md).

## 1b. TEA key retrievable from the cloud by BLE name (weak authorization)
The vendor endpoint `POST /bluetooth-challenges` returns a device's **16-byte TEA key** given only its BLE
advertising name (`btChallengeId = OBI-XXXXXX`) and **any valid OBI login** — it does **not** verify that
the requesting account owns (or has ever enrolled) that device. Since the `OBI-XXXXXX` name is
broadcast in the clear in every BLE advertisement, anyone in radio range (or who can otherwise observe/guess its
name) and holds any OBI account can pull the key that protects its BLE control channel. Combined with #1,
the per-device TEA key is effectively low-secrecy. Owners: prefer the UART path and treat the BLE key as
not-secret. See [cloud-api.md](cloud-api.md#where-the-device-secrets-come-from).

## 2. LoRa link crypto — outer XOR on every frame, but old readers are TEA-encrypted, **not** XOR-only
Every frame's `byte3..end` is obfuscated with a **single-byte XOR** whose key is the byte-sum of the
cleartext 3-byte handle — derivable from any captured frame. So the handle, the `type/cmd` byte and the
plaintext control-frame payloads are effectively public on **every** firmware. That part is real.

> **Correction (verified by building the ESP32 gateway).** An earlier version of this note claimed the
> *energy payload* also stayed 1-byte-XOR on `1.0.x`/`3x.x`, on the assumption that the ECDH secret "went
> unused." Pairing a real reader with our own gateway ([../open_obi_energy_meter](../open_obi_energy_meter))
> disproved that. The old reader the cloud reports as firmware **`1.0.1`** in fact reports **softver 32
> ("v32")** on the LoRa link and runs the **same ECDH → TEA-ECB** crypto as 1.2.x. Reversed from the v32
> reader's ack handler `sub_B7A0`, that reader **requires** its energy ACK (cmd 24→56 / 25→57) to be
> **TEA-encrypted** with the ECDH-derived key — it rejects anything that isn't a multiple of 8 or doesn't
> decrypt — and its energy uplink decodes as TEA as well (`tea/legacy` in `main.cpp`). Only the **frame /
> command layout** differs from 1.2.x (legacy energy layout, cmd 24/25 energy + 56/57 acks, cmd 17/49
> announce/bind) — **not** the encryption.

**On 1.2.x** the mechanism is identical: `lora_cmd32_key_exchange` takes the reader's P-256 public key,
computes a 32-byte shared secret (`ecdh_compute_shared_secret`) and stores it per device; `lora_decrypt_frame`
/ `lora_encrypt_frame` run **TEA-ECB** over the payload with that key (device struct: `+78` key-ready flag,
`+79` reader pubkey 64 B, `+143` shared key 32 B). So on **v32 and 1.2.x** the LoRa energy payload is
TEA-encrypted with a per-device ECDH key — the read/forge/inject scenario from the old XOR-only claim does
**not** apply to these readers. The real residual weakness is that **the ECDH join itself is
unauthenticated**, so an active MITM *during pairing* could still establish its own key (on both
generations). See [lora-protocol.md](lora-protocol.md).

## 3. BLE fragment reassembly — heap overflow (partial finding)
`ble_reassemble_frags` allocates a **fixed 5120-byte** buffer on the first fragment and then appends each
subsequent fragment with `memcpy(buf + 4 + total, frag, frag_len)`, updating `total += frag_len`.
Fragments are only checked for **sequence** (packet number matches, index = previous+1) — the **total
reassembled length is never checked against the 5120-byte buffer**.

```
first frag  -> malloc(5120)
each frag   -> memcpy(buf + 4 + total, frag, len); total += len   # no bound on total
```
A message split into ~30+ fragments of ~173 bytes each overflows the buffer (heap overflow). The index
field is a byte, so up to 256 fragments (~44 KB) can be pushed at a fixed 5120-byte allocation.

**Reachability / caveat:** reassembly runs **after TEA decryption**, so triggering it needs a valid
TEA-authenticated fragment stream — i.e. the device's TEA key (which is itself obtainable, see #1). So
this is an *authenticated* heap overflow. It has **not been weaponised/tested here** — it is documented as
a partial finding; exploitability (heap layout, allocator, what follows the buffer) still needs work.

## 4. eFuse USER_DATA key printed to the boot log
At boot the firmware reads a 16-byte eFuse USER_DATA value and logs it ("Key content") to UART at INFO
level. Capturing the boot log can reveal it. See [uart-config-protocol.md](uart-config-protocol.md).

## 5. Locked bootloader (a hardening, for context)
eFuses disable JTAG and ROM download mode on provisioned units, so you cannot reflash over UART/JTAG —
custom firmware must go through the cloud OTA path. See [firmware-layout.md](firmware-layout.md#boot-hardening-locked-bootloader).

---
These notes are for securing and self-hosting your own hardware. Everything above assumes a device you own.
