# v57 / v90 reader: 1-second live readings (experimental)

> ➡️ **Deutsche Version: [de/README.md](de/README.md)**

> ✅ **Verified on one reader** (2026-11-06): `reader_modded_live_91mock_v92_EMHeHZP.bin` on a USB-powered
> hardver-6 reader at a Landis+Gyr E320 (SML push every second). The reader had run the legacy v32 before,
> and the gateway's legacy bootloader path flashed it straight to v57 without re-pairing. Measured over
> 61 energy frames at the gateway: median spacing **1.00 s** (max 2.0 s, every frame ACKed); stock v57
> manages ~10 s. The other six variants carry the same three patches but are not tested individually.

A live variant of every v57-based image in this folder (`reader_stock_v57.bin` and the six
`reader_modded_89mock_v90*.bin`). In live mode (interval 1–2 s) the reader should send about one reading
per second at a meter that pushes SML every second. Stock v57 manages about one every **10 s**. Other
intervals behave exactly as before.

## What holds v57 back in live mode

1. **Meter read cycle.** The cycle is the same one the v32 fix addresses (see
   [../legacy_v32/README.md](../legacy_v32/README.md)). The reader takes one telegram, then waits in
   state `0x168` until `(now_s - start_s) > period`. That check is `sub_B7C8`, the period is the upload
   interval, and so the pause costs 2–3 s even in live mode.
2. **LoRa window.** The reader only transmits inside a ~100 ms window it opens **9950 ms** after the last
   gateway beacon it heard (`reader_lora_timing_sm`, constant at `0xB9C0`, see
   [../../03-reverse-engineering/lora-protocol.md](../../03-reverse-engineering/lora-protocol.md)). The
   gateway beacons at 1 Hz, so the reader listens to only every tenth beacon.

## The patch

Three 4-byte sites are redirected by BL to a 64-byte, position-independent blob ([live.S](live.S))
appended to the image:

| Site | Stock | Hook |
|---|---|---|
| `0xD11E` | `bl sub_B7C8` (cycle wait) | `entry_cycle_due`: due at once when period ≤ 2, otherwise tail-jumps to the stock check |
| `0xB942` | `ldr r1,=9950; movs r0,#0` (next window) | `entry_window_base`: 950 ms when interval is 1–2, otherwise 9950 |
| `0xB906` | `ldr r1,=9950; adds r0,r0,r1` (retry window) | `entry_window_retry`: same choice, plus the retry offset |

- The interval comes from `0x20000114`, the value the gateway's energy ACK sets (getter `sub_79E8`).
  With interval 1–2 the reader listens to **every** beacon (window at +950 ms, beacon at +1000 ms).
  An interval of 0 (not set yet) keeps the stock window.
- The blob calls the stock check by absolute address. Images run at **`0x4800`**: the repo numbering
  is file offset + `0x4000`, so `sub_B7C8` is at runtime `0xBFC8`. `build.sh` fails if the blob ever
  needs a relocation.
- softver 57/89 → **91**, file names `…_live_91mock_v92…`. Flashing back to a v90 file still works,
  because the gateway advertises 90 and the reader reports 91.
- Size: the largest live image ends at runtime `0xF780`, well below `0xFC00`. The legacy v32 test
  showed that the bootloader refuses images that reach that last flash page.

## Build

```bash
cd reader_firmware_mod/live_v57
./build.sh                          # needs arm-none-eabi-gcc
python3 patch_live.py --all         # all seven images -> *_live_91mock_v92*.bin
python3 patch_live.py ../reader_modded_89mock_v90_DWSB20_2TH.bin   # just one
```

`patch_live.py` only accepts the exact committed images (SHA-256 list in the script). When the v90
release files are rebuilt with different bytes, update that list. The live images are not committed;
CI builds them, and `patch_live.py` recreates them byte-identically.

Flash as usual (dashboard → reader card → **Flash firmware**), then set the reader's interval to
**Live**.

## Caveats

- **USB power only.** In live mode the reader wakes for every beacon, keeps reading the meter and
  transmits about once a second. On batteries, don't use live mode. Other intervals are unchanged.
- **One live reader per gateway.** v57 readers transmit right before the beacon. Several live readers
  would share the same ~50 ms slot every second and collide.
- **Airtime.** At SF7 a 1 Hz reader uses roughly 2 % of the sub-band's 10 % duty cycle, more at SF9.
