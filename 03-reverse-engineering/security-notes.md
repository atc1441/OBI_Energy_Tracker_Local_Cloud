# Security notes

Factual weaknesses found while reversing, so **owners can assess and self-host** their units. Ordered by
what matters for ownership, not by severity theatre.

## 1. TEA key readable/writable over UART (plaintext)
The UART0 config channel (`C5 5C …`) has **no authentication**. `cmd 49` returns the 16-byte TEA key,
`cmd 48` sets it, and `cmd 58/59` write/read WiFi credentials — all in the clear. Anyone with access to
the console pins (GPIO20/21) owns the BLE channel and the WiFi. This is also the intended self-host entry
point. See [uart-config-protocol.md](uart-config-protocol.md).

## 2. LoRa link has no real crypto
Frames are obfuscated with a **single-byte XOR** whose key is the byte-sum of the cleartext 3-byte handle
— derivable from any captured frame. The ECDH exchange (cmd 32) gates data flow but its shared secret is
never used. Result: LoRa traffic can be read, forged, and injected (e.g. fake energy readings, or pulling
the stored reader firmware via unbound cmd 21). See [lora-protocol.md](lora-protocol.md).

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
