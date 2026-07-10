# reader_sdk — a C-hook framework for the BAT32G135 reader firmware

> ➡️ **Deutsche Version: [de/README.md](de/README.md)**

Goal: instead of hand-translating every change to the (closed-source) vendor
firmware into Thumb-1 opcodes, write the hook code in C, compile it with a
real `arm-none-eabi-gcc` (Cortex-M0+/ARMv6-M, Thumb-1, no FPU), and splice the
result into the firmware.

## Folder layout

- `reader_stock_v57.bin` — unmodified original firmware (the source
  `splice.py`/`splice_DWSB20_2TH.py` patch).
- `reader_modded_v87.bin` — finished result: just the int24 fix (SML
  TL=0x54), softver set to 87, IDA-verified. This is the file to flash for
  meters that only needed the int24 fix (power showing "n/a").
- `reader_modded_v87_DWSB20_2TH.bin` — the DWSB20.2TH negative-power fix (see
  below), softver ALSO set to 87 (matching `reader_modded_v87.bin`'s
  release version — the build was live-verified under a temporary softver
  of 119 during development, then renumbered to 87 for release; see the
  comment in `splice_DWSB20_2TH.py`), built by
  `./build.sh DWSB20_2TH && python splice_DWSB20_2TH.py`. Flash this instead of
  plain v87 for meters whose negative/feed-in power reads as a large bogus
  positive number instead of negative. Live-verified on real hardware
  across sustained periods in both directions and across a real
  import↔export transition (see "Live verification" below). Since both
  release files report softver 87, don't rely on the gateway's reader-OTA
  path to switch a reader from one variant to the other (same reported
  version looks like "no update" to that logic) — flash the variant your
  meter needs directly.
- `hooks.c`, `entry.S`, `link.ld`, `vendor.h`, `build.sh`, `splice.py`,
  `splice_DWSB20_2TH.py` — shared source and build scripts. `hooks.c` has
  the DWSB20.2TH fix guarded behind `#ifdef FIX_NEGATIVE_POWER`, which
  `./build.sh DWSB20_2TH` defines; `./build.sh` (no argument) builds the
  plain int24-only variant. Both read the SAME `hooks.c`/`entry.S`, so a
  change to the shared int24 decoder logic only needs to be made once.
  Both are the durable, repeatable release process going forward — run
  both build+splice pairs whenever `hooks.c`/`entry.S` changes.
- `fix_negative_power.py` — standalone patch script for an early,
  abandoned approach (SML Int8/Int16 dispatch-table retargeting). Kept
  only for the historical record — confirmed NOT the bug the DWSB20.2TH
  actually hits.
- `build/` — ONLY compiled intermediate files: `hooks.*` (plain variant)
  and `hooks_DWSB20_2TH.*` (DWSB20.2TH variant), kept side by side so
  building one doesn't clobber the other. `build.sh` creates the folder
  itself if needed.

## Building blocks

- `hooks.c`       — the actual hook functions (plain C, AAPCS, normal
                    prologues/epilogues — the compiler handles those).
- `entry.S`        — tiny glue trampolines (hand-written asm, a few lines
                    per hook). These adapt the specific vendor call site's
                    register convention (e.g. "R4 = output pointer, R0 =
                    return value, ends with `pop {r3-r7,pc}`") and then do
                    a normal `bl` into the C function. A new hook only
                    needs a few lines here.
- `link.ld`        — linker script that places the code at a fixed address
                    in the free flash space right after the firmware image
                    (currently: reader_meter_v57.bin, 44552 B -> base
                    0x4000 + 44552 = 0xEE08).
- `build.sh`       — compiles+links to `hooks.elf`, extracts `hooks.bin`
                    (raw machine code) and `hooks.map`/`hooks.sym` (symbol
                    addresses for splice.py). Pass `DWSB20_2TH` as the first
                    argument to additionally build the DWSB20.2TH fix
                    (`-DFIX_NEGATIVE_POWER`) into `hooks_DWSB20_2TH.*` instead.
- `splice.py` / `splice_DWSB20_2TH.py` — append the matching `hooks*.bin` to
                    the firmware image and patch the call sites (BL
                    encoding computed programmatically) to jump into the
                    entry points from `entry.S`. Each also bumps the
                    embedded softver bytes — **always to a fresh,
                    never-before-used value** when iterating: the
                    reader-OTA path silently skips re-flashing when the
                    embedded/advertised softver matches one already tried,
                    which otherwise looks exactly like "the fix doesn't
                    work" against genuinely stale code.

## Adding a new hook

1. Write the function in `hooks.c` (plain C, no libc other than what
   `vendor.h` declares). Freestanding `-nostdlib` build — no soft-division
   runtime is linked, so avoid `%`/`/` on non-power-of-2 values.
2. If the call site doesn't use plain AAPCS (e.g. a vendor-specific
   register convention): add a small trampoline stub in `entry.S` that
   moves the registers into AAPCS arguments (R0,R1,R2,R3), `bl`s into the
   C function, then performs whatever return sequence the call site
   expects (e.g. `pop {r3-r7,pc}`). Remember to add `-DFIX_NEGATIVE_POWER`
   guards to entry.S sections too if the hook is DWSB20_2TH-only, and `KEEP()`
   its section in `link.ld` so `--gc-sections` doesn't drop it.
3. Add the call-site address + the entry.S symbol name to `HOOKS = [...]`
   in `splice.py`/`splice_DWSB20_2TH.py`.
4. `./build.sh [DWSB20_2TH] && python splice.py|splice_DWSB20_2TH.py` — produces
   a new, patched `.bin`. Bump the softver patch byte to a fresh value
   first (see above).
5. **Always** cross-check with IDA (`idalib_open` + `disasm`) before
   flashing — splicing itself only validates byte encoding, not semantics.
   Direct Python byte-reads of the flashed `.bin` are a reliable fallback
   when IDA's headless session gets flaky.

## DWSB20.2TH negative-power fix

Confirmed byte-for-byte with a real `meter_sniffer` capture of OBIS
1-0:16.7.0 (power): the meter's own SML encoder fails to sign-extend a
negative value into the 24-bit field (SML `Integer24`, TL=0x54) when the
correct top byte would be exactly `0xFF` — it sends `0x00` there instead.
Captured example: raw value bytes `00 EF 4E` decode to +612.62 W, but
`FF EF 4E` (-42.74 W) was the physically correct reading for that same
moment. At this meter's scaler (-2, confirmed from the same capture),
that's a fixed **+655.36 W** (2^16 raw units) offset — this is the same
bug class documented in a public report on this same DWSB20.2TH model
([mikrocontroller.net thread](https://www.mikrocontroller.net/topic/564786)),
where the community's own workaround uses the exact same three correction
brackets (2.56 / 655.36 / 167772.16 W) this fix's single 655.36 W
correction is the first of.

The bug is on the wire before the reader ever decodes it — no amount of
re-parsing recovers a bit that was never sent, and (confirmed live) the
magnitude of the received value alone does NOT reliably distinguish a
genuinely small positive reading from a corrupted negative one on this
reader's actual code path (see "abandoned attempts" below) — so the fix
instead cross-checks against the only other ground truth available: the
cumulative import/export Wh counters, which are unaffected by this bug
(reported as plain, correctly-signed-by-construction unsigned deltas).

### Active code path: `sub_77B4`/`sub_9ED8`

Two candidate SML-telegram parsing paths exist in this firmware
(`sub_AA7C`'s dispatch, cases 1 vs 2/3). Live diagnostics (unconditional
sentinel values written to the transmitted energy payload) confirmed
`sub_77B4`/`sub_9ED8` (case 1) is the path this reader actually uses for
live power reporting — the other path (`sub_7434`, case 2/3) is dead code
for this reader/meter combination.

`hook_power_correct2` hooks `sub_77B4` at 0x7800, right after
`BL sub_9ED8` — R0 already holds `sub_9ED8`'s fully-computed, already
Watt-scaled power value (this path has no separate scaler-conversion call
the way `sub_7434`'s `sub_4700` does). It reads the reader's own live
import/export counters (`unk_20000D68`/`unk_20000D6C`, the same struct
`sub_77B4` fills) and maintains a small persistent state machine in scratch
RAM (`0x20001000`-`0x20001010`, empirically verified safe — not all
apparently-unused RAM addresses actually are, see comments in `hooks.c`):

- **Sticky direction latch with aging**: because the Wh counters only tick
  once every several seconds to minutes at typical power (far less often
  than the ~6 s power-report interval), a strict same-telegram "did the
  counter move export-wards THIS telegram" check misses almost every
  genuinely-negative reading. Instead, remember which counter ticked LAST
  (import or export) and keep applying the correction for a bounded number
  of subsequent quiet telegrams (`STICKY_MAX_AGE`, currently 5) before
  reverting to "unknown" (no correction applied).
- When the latched direction is "export" and the raw value is in the
  plausible corrupted-positive bracket (`0 <= raw < NEG_FIX_W`, 655 W),
  subtract `NEG_FIX_W` to recover the true negative value.

`hook_power_correct` (hooked into the now-confirmed-dead `sub_7434` path
at 0x75EE) is kept as a harmless no-op — cheap insurance in case some
telegram variant does reach it after all, without interfering with
`hook_power_correct2`'s state.

### Known, accepted limitation

A genuine direction change isn't visible to this fix until the FIRST tick
of the NEW direction's counter arrives — ticks are the only ground truth
available at this injection point. This means a real transition (e.g.
import-dominant switching to export-dominant) can show up to
~`STICKY_MAX_AGE` telegrams (order of a minute, depending on report
interval) of stale/incorrect sign before catching up. This has been
observed live and is a physical limitation of relying on cumulative
counters as the only available signal — not something fixable without a
different signal source, which does not appear to exist in the data
available to the reader firmware at this point in the pipeline.

### Live verification

Verified on real hardware (OBI C3 gateway + a paired DWSB20.2TH reader):
- Multiple, separately-observed sustained windows (minutes-long) of
  correct positive power during genuine import-dominant activity (import
  counter ticking, export flat), with no false-negative misfires.
- A real export→import transition captured end-to-end in the gateway's
  own history log: a single export tick correctly triggered the
  sticky latch (small negative readings, magnitude-consistent with
  `NEG_FIX_W`, matching the documented correction bracket — not a wild/
  wrong-unit value), which then correctly aged out and reverted to
  accurate positive readings once import resumed ticking and stayed
  correct for the following 10+ minutes.
- Reflashed via reader-OTA and confirmed the embedded softver (tested as
  119 during development, since renumbered to 87 for release — see above)
  persists across a gateway reboot and reader re-pair.

### Things that were tried and abandoned

Kept here so they aren't re-attempted blind:

- **Byte-scan inside `hook_decode_int24`** (multiple variants: fixed -16
  offset, then a wider 4–24 byte backward scan, then a tight unit/
  scaler-adjacent check) trying to identify "this Int24 value is OBIS
  16.7.0" from inside the low-level SML decoder, before applying a raw
  -65536 correction. Each variant was proven correct via a byte-for-byte
  Python replay against the one real `meter_sniffer` capture, but NONE
  fired reliably live, even after ruling out stale-OTA caching. The exact
  byte layout preceding the value field isn't as constant as that one
  capture suggested.
- **Patching `sub_7434`'s tail** (0x7604), AFTER its output struct was
  already filled: the trend logic itself worked (verified via a temporary
  debug build), but the write never survived to the transmitted energy
  payload (confirmed with a temporary gateway-side raw-payload debug
  field — since removed). Something downstream reads the struct via a
  path this doesn't intercept — and this path turned out to be dead code
  for this reader/meter anyway (see above).
- **A third hook at `sub_BEE8`'s callback-dispatch entry** (0xBEEC),
  attempting to catch the value one step later. Reliably triggered a
  silent OTA rollback (full image transferred, reader silently kept
  running the old version) on repeated attempts, including with a
  defensive null/range-checked pointer read — cause not understood, not
  worth the risk once the `sub_77B4`/`sub_9ED8` path fix worked instead.
- **An unconditional "magnitude alone proves it's negative" rule**
  (`raw < 330` implies negative, on the theory that a genuinely small
  positive reading would go out via the shorter Int16 encoding and never
  reach this decoder). Confirmed live this does NOT hold on the
  `sub_9ED8` path: watched a clearly import-dominant stretch (import
  ticking, export static) where small genuinely-positive values (7–27 W)
  still arrived through this same decoder, and the rule flipped them
  negative too. Reverted in favor of the trend-only logic above.
- A fixed-offset scratch RAM address that appeared unused via static IDA
  xref analysis (`0x20000D10`, then `0x20000D20`) never actually
  persisted state across calls live, even with `volatile` — something
  else (likely stack) overwrites it between hook invocations. Relocated
  to `0x20001000`+, empirically confirmed safe.
- A plain (non-`volatile`) pointer to the scratch persistence state let
  the compiler treat the later re-read as predictable from the earlier
  write and optimize/reorder around it — the magic word never survived to
  the next call. Fixed by declaring all scratch pointers `volatile`.

Both classes of lesson point the same way: prefer fixing things at the
single, actually-live code path (confirmed via a live sentinel, not just
static analysis) using ground-truth signals that are themselves
unaffected by the bug, over trying to intercept or override a value
further downstream in a pipeline that isn't fully mapped.

## Why a glue stub instead of a direct BL into the C function?

The vendor firmware's patch sites often do NOT follow plain AAPCS (e.g. the
return value goes through a pointer in R4 instead of R0, or a shared
function epilogue uses `pop {r3-r7,pc}` instead of `bx lr`, or LR needs to
be pushed/popped around an extra call the original single instruction
never made). A short glue stub captures that cleanly, while the actual
hook logic stays plain, compiler-checked C. This keeps the hand-written
assembly minimal and confined to one spot per hook.
