# Legacy v32 reader: 1-second live readings

> ➡️ **Deutsche Version: [de/README.md](de/README.md)**

A patch for the **legacy reader** (cloud firmware "1.0.1", softver **32**, shown as `legacy` on the
gateway dashboard). In live mode, the patched reader sends **every meter telegram**, about one per
second on a meter that pushes SML every second. The stock reader sends one every **3–4 s**.

| Measured on one reader (hardver 6) at a 1 Hz SML meter | stock v32 | patched |
|---|---|---|
| energy frames at the gateway (`/api/radio`), median spacing | 3.8 s | **1.00 s** |
| meter telegrams per report | 1 of every 3–4 | every one |
| Home Assistant via BTHome (proxy scan window 300 ms), median | ~4 s | **1.02 s** |

Live-verified on 2026-11-05 (one reader, USB-powered, 276-byte SML telegram, 9600 baud).

## Why the stock reader is slow

The reader's meter-read state machine (repo numbering `0xD624`, state struct `0x200001B0`) works in
cycles. It enables the meter UART, takes **one** telegram and disables the UART again. It then waits in
state `0x114` until `(now_s - start_s) > period`, checked by `sub_C074` in whole seconds. `period` is
the **upload interval** (`S[8]`, the value the gateway sends in the energy ACK). The gateway's "Live"
preset arrives as interval 2 (`2 × b[1]`, see the legacy ACK in `open_obi_energy_meter/src/main.cpp`),
so the pause alone is 3 s. Then the reader waits for the next telegram. The result is 3–4 s per
reading, even though the reader never sleeps on USB power and the meter sends every second.

Diagnostic builds narrowed it down. They wrote counters into the five unused pad bytes (19–23) of each
energy frame, so the gateway's radio log shows them decrypted. Per report the reader received exactly
one telegram (276 bytes), dropped nothing and never slept.

## The patch

One call is redirected: `bl sub_C074` at `0xD7DE` → `entry_cycle_due` ([live.S](live.S)). For
`period <= 2` the hook reports "due" at once, so the next read cycle starts right after the previous
telegram. For any longer interval it calls the stock check, and the stock behaviour (and power saving)
stays unchanged.

- **No room to append.** The bootloader rejects an image that reaches the last flash page. A test image
  that ended at runtime `0xFC68` was pulled again and again and never written; the stock image ends at
  `0xFBC4` (60 bytes below `0xFC00`). The 16-byte hook therefore lives in dead code:
  uECC's `vli_mmod_fast_secp256k1` (`0xE6EC`–`0xE773`). Only the secp256k1 curve struct points to it,
  and nothing references that struct, because the reader only uses secp256r1 (getter at `0xDD54`). The
  patched image has exactly the stock size.
- **Version.** softver 32 → **92** (frame byte at `0xD244` and the own-version check in the ACK handler
  at `0xAFE8`), and the file is named `…v93`. That keeps the gateway's "same version" check out of the
  way in both directions: flashing the stock image back (`…v32`) works too.
- **Address numbering.** Addresses use this repo's numbering (file offset + `0x4000`). The image
  actually runs at **`0x4800`**: the vector table is at file offset 0 and the reset handler at file
  offset `0xD8`. Relative BLs don't care, but absolute code addresses (function pointers) in this
  numbering are off by `0x800`.

## Build and flash

No vendor binary ships here. Bring your own stock v32 image, or anything that contains it. A stock bridge
**1.0.x** app partition or a full bridge flash dump carries it. Right after the stock → custom
conversion, the other OTA slot still holds the stock bridge image (see the debug page's flash dump).

```bash
cd reader_firmware_mod/legacy_v32
./build.sh                                       # needs arm-none-eabi-gcc
python3 patch_legacy_live.py <image-or-dump>     # -> reader_legacy_live_92mock_v93.bin
python3 patch_legacy_live.py <image-or-dump> --extract-only   # -> reader_legacy_stock_v32.bin (revert)
```

The script finds the image by its vector table, cuts it at the end of the scatter-load table and checks
its SHA-256 (`4dfb9c94…b8e2`, 46,020 bytes). Any other build is refused. The output is byte-identical
to the image verified live (`10f8ee15…6d48`).

Flash it like any reader image: dashboard → reader card → **Flash firmware**. Then set the reader's
interval to **Live** (or 2-3 s), which the gateway sends as `b[1] = 1` → interval 2.

> **Gateway heap:** the gateway holds the whole image in one buffer. With BTHome enabled, the largest
> free block can be smaller than 46 KB, and the upload then fails (`{"ok":false}`). Disable BTHome,
> reboot the gateway, flash the reader, then enable BTHome again (another reboot).

## Caveats

- **USB power.** In live mode the reader keeps its meter UART on and transmits about once a second.
  On USB that costs nothing. On batteries, avoid live mode with this patch (other intervals are
  unchanged).
- **Airtime.** One reader at 1 Hz uses roughly 2 % of the 869.5 MHz sub-band's 10 % duty cycle. v32
  readers transmit on their own clock, so several live readers on one gateway will occasionally
  collide. Lost ACKs make the reader retry after ~5 s (seen as rare 5–7 s gaps).
- **Scope.** This is v32 only. The v57/v90 readers have the same cycle logic plus their own ~10 s
  LoRa window, and they need a separate port.
