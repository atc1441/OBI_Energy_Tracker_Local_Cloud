# reader_sdk — a C-hook framework for the BAT32G135 reader firmware

> ➡️ **Deutsche Version: [de/README.md](de/README.md)**

Goal: instead of hand-translating every change to the (closed-source) vendor
firmware into Thumb-1 opcodes, write the hook code in C, compile it with a
real `arm-none-eabi-gcc` (Cortex-M0+/ARMv6-M, Thumb-1, no FPU), and splice the
result into the firmware.

## Folder layout

- `reader_stock_v57.bin` — unmodified original firmware (the source
  `splice.py`/`splice_DWSB20_2TH.py` patch).
- `reader_modded_89mock_v90.bin` — finished result: just the int24 fix
  (SML TL=0x54), IDA-verified. This is the file to flash for meters that
  only needed the int24 fix (power showing "n/a").
- `reader_modded_89mock_v90_DWSB20_2TH.bin` — the DWSB20.2TH negative-power
  fix (see below), built by
  `./build.sh DWSB20_2TH && python splice_DWSB20_2TH.py`. Flash this
  instead of the plain variant for meters whose negative/feed-in power
  reads as a large bogus positive number instead of negative.
  Live-verified on real hardware across sustained periods in both
  directions and across real import↔export transitions (see "Live
  verification" below).
- `reader_modded_89mock_v90_ISKRA_MT691.bin` — the ISKRA MT-691 "nothing
  decodes at all" fix (see "ISKRA MT-691 fix" below), built by
  `python splice_ISKRA_MT691.py` (reuses the plain build's `hooks.bin` —
  no separate `build.sh` variant needed). Flash this instead of the plain
  variant for an ISKRA MT-691 whose readings never show up at all. **NOT
  live-verified** — built entirely from static analysis of a third-party
  SML capture; no ISKRA MT-691 hardware or sniffer was available to this
  project to confirm it in practice (see that section for details and
  confidence level).

  **Naming/versioning is deliberate, and identical across all three
  release files**: each firmware's OWN reported softver is 89, but the
  RELEASE is named/tagged "v90" — a permanent "mock version" mismatch, not
  a leftover dev artifact. The gateway's web UI parses the OTA target
  version to advertise from the uploaded filename (`/v(\d+)/`, first
  match — here "v90"), and only treats an advertised version as a no-op
  if it equals the reader's own currently-reported softver (or 0). Since
  90 can never equal any of these files' own reported 89, any release
  file can be re-uploaded/reflashed any number of times — e.g. to force a
  reader back onto a known-good build, or to switch a reader from one
  variant to another — without ever hitting the "already this version,
  skipping" OTA no-op a same-numbered file would. If you build a further
  local iteration of any variant with `splice.py`/`splice_DWSB20_2TH.py`/
  `splice_ISKRA_MT691.py`, keep both numbers matched to what's actually
  being tested (see the comment above each script's softver patch) — the
  89/"v90" mismatch is only for the pinned release builds.
- `hooks.c`, `entry.S`, `link.ld`, `vendor.h`, `build.sh`, `splice.py`,
  `splice_DWSB20_2TH.py`, `splice_ISKRA_MT691.py` — shared source and
  build scripts. `hooks.c` has the DWSB20.2TH fix guarded behind
  `#ifdef FIX_NEGATIVE_POWER`, which `./build.sh DWSB20_2TH` defines;
  `./build.sh` (no argument) builds the plain int24 + SML-Int16-sign-fix
  variant that `splice.py` AND `splice_ISKRA_MT691.py` both start from
  (the ISKRA fix is a pure additional byte patch on top, no extra C hook
  needed). All variants read the SAME `hooks.c`/`entry.S`, so a change to
  shared logic (int24 decode, the Int16 sign fix) only needs to be made
  once and propagates to every variant on the next build. These are the
  durable, repeatable release process going forward — run
  `./build.sh && python splice.py`, `./build.sh DWSB20_2TH && python
  splice_DWSB20_2TH.py`, and `python splice_ISKRA_MT691.py` whenever
  `hooks.c`/`entry.S` changes.
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

## SML Integer16 sign-extension fix (`entry_sxth16_fix`)

Applies to **every** variant unconditionally (built into `hooks.bin` itself,
not gated behind `FIX_NEGATIVE_POWER`) — this is a general firmware defect
in the vendor's own SML value decoder, not a workaround for any one meter's
encoding quirk.

`sub_C0A4` (the low-level SML TL-byte value dispatch) has a case for
Integer16 (`TL=0x53`) that reads the two value bytes and computes the
result as `(byte0<<8)+byte1` — a plain **zero**-extend into the wider
register used for the rest of the pipeline, with no sign-extension step.
The adjacent, near-identical case for `TL=0x63` does the same byte read but
follows it with an `SXTH R0,R0` (sign-extend halfword) before computing the
sign for the high dword — the 0x53 case is simply missing that one
instruction. Traced all the way through the downstream conversion chain
(`sub_9ED8` → `sub_C41C`/`sub_46D4`/`sub_4504`/`sub_475C`, scaler and
sign/magnitude handling) and confirmed nothing later recovers the correct
sign — `sub_46D4` derives the sign purely from the high dword sub_C0A4
handed it, which is always 0 for this case. The result: a raw Integer16
value like `0xFE5F` (-417 decimal, itself perfectly correctly encoded on
the wire) is reported as **+65119**, not -417 — the value's bit pattern is
reinterpreted as unsigned, not merely sign-flipped.

This can affect ANY SML field using plain Integer16 that carries a
negative value — most relevantly, a meter's power (16.7.0) reading during
feed-in, if that meter (unlike DWSB20.2TH, which uses a wider, 24-bit
field with its own separate encoding bug — see below) encodes power as a
correctly-signed Integer16 that this reader then fails to decode as
signed. Confirmed against a real capture from a third-party report (an
ISKRA MT-691 meter, see "ISKRA MT-691 fix" below) where this was the exact
failure mode for a -417 W reading.

**Fix**: `entry_sxth16_fix` (`entry.S`) replaces the 4-byte
`asrs r1,r0,#0x1f ; str r1,[r4,#4]` at `0xC110` (inside `sub_C0A4`'s 0x53
case) with a `BL` into a small trampoline that does the missing
`sxth r0,r0` first, then redoes the sign computation and the store the
original instructions performed, then returns via `bx lr` (not `pop`,
since this isn't a jump-table-slot replacement — it's a straight-line
mid-function patch that must resume the untouched code right after it,
which handles the low-dword store and the branch to the shared switch
epilogue). No C hook needed — the fix is pure ALU, small enough to write
directly in `entry.S`.

**Confidence**: high for the decode-level analysis (byte-exact trace of
both the buggy and the sibling-correct case, confirmed via direct
`get_bytes` reads, not summarized from memory) and for the patch-site
safety (confirmed via `xrefs_to` that nothing branches into the middle of
the 4 replaced bytes; confirmed R0/R1 are dead after this case in the
shared epilogue at `loc_C26C`, so free to clobber; confirmed LR is treated
as pure scratch mid-function in `sub_C0A4`, never read as a live register
between entry and the function's real exits, so the trampoline's own `bl`
is safe without a push/pop). **Live-verified** on real hardware: flashed a
build containing this hook onto the DWSB20.2TH test reader and confirmed
normal decode continued working correctly across a full negative↔positive
transition — this doesn't exercise the SPECIFIC Integer16 negative-value
path (DWSB20.2TH uses Integer24, not Integer16, for power), but it does
confirm the hook itself doesn't destabilize `sub_C0A4` or the surrounding
dispatch for a reader actively decoding real telegrams.

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
- **Second bracket (2026-07-11)**: confirmed live that feed-in beyond
  roughly 655 W surfaces via a much larger wraparound instead — e.g. a
  real ~-720 W moment showed up as +167053..+167098 W, which is
  physically impossible for a residential meter. This is the same bug
  class one level up: values whose magnitude no longer fits in 16 bits
  apparently go out via the meter's existing Int32 field (SML TL=0x55,
  already correctly handled by the stock firmware — no missing case here,
  unlike Int24) instead of Int24, and the same "new leading byte wrongly
  written as 0x00 instead of 0xFF" bug now corrupts the top byte of a
  32-bit rather than a 24-bit value, giving a 2^24 raw-unit offset
  (167772.16 W at this meter's scaler) instead of 2^16 (655.36 W).
  Reproduced against the gateway's own history log: the implied true
  values (decoded value minus 167772) matched the concurrent Wh-tick rate
  on the export counter almost exactly. Unlike the first bracket, this
  one needs no `dir`/latch gate at all — no real residential reading ever
  approaches 167772 W (or even a tenth of it) under any interpretation,
  so any decoded value at or above `IMPLAUSIBLE_W` (50000 W — comfortably
  above any real reading and comfortably below the lowest value this
  bracket's corruption can produce, ~83886 W) is unconditionally
  corrected by subtracting `NEG_FIX_W3` (167772 W), regardless of the
  sticky latch's current state — which also sidesteps that latch's
  inherent lag for this case.

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
- The second bracket fix verified live immediately after deployment:
  feed-in in the 550-720 W range (previously showing as +167xxx W)
  displayed correctly as negative from the first reading, and a
  subsequent real export→import transition (feed-in down to positive
  import readings climbing from ~200 W to ~1400 W) showed no false
  triggers on either bracket in either direction.
- Reflashed via reader-OTA and confirmed the embedded softver (tested as
  119, then 120 for the second-bracket fix during development, since
  fixed at 89 for the "v90"-named release — see above) persists across a
  gateway reboot and reader re-pair. Also confirmed the mock-version
  mismatch itself works as designed: uploading the "v90"-named file
  (advertising ver=90) to a reader already on this exact build (which
  reports softver 89) still triggered a full reflash rather than being
  silently skipped as a no-op.

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

## ISKRA MT-691 fix

**⚠️ NOT live-verified.** Built entirely from static analysis (IDA
decompile/disasm) of a real SML capture supplied secondhand by a user
relaying a report from someone else — no ISKRA MT-691 hardware, meter
sniffer, or paired reader was available to this project to confirm the
fix in practice. Treat as best-effort until someone confirms it working
(or not) against a real MT-691.

**The bug**: with an ISKRA MT-691 meter, NOTHING is decoded/displayed —
not a sign/magnitude issue like DWSB20.2TH, total silence. The supplied
capture was manually decoded and confirmed fully standards-compliant SML
with no defect of the meter's own encoding: a clean `SML_GetList.Res`
with entries for a manufacturer-ID octet string ("ISK"), a serial number,
then OBIS `1-0:1.8.0` (import, Unsigned32 + scaler -1), `1-0:2.8.0`
(export, same), and `1-0:16.7.0` (power, Integer16, correctly
two's-complement-signed value -417). Every value type involved
(Unsigned32/Integer16/OctetString) is already supported by the stock SML
value-type dispatch, and the generic OBIS matcher (`sub_B0C8`) walks
variable-length fields correctly regardless of this meter's unusual
leading manufacturer-ID/serial entries — so nothing about the meter's
telegram itself is at fault, unlike the DWSB20.2TH case.

**Root cause**, fully traced via IDA (addresses verified against the
actual binary, not assumed from names): `sub_9DA0` — the import/export
multi-tariff (OBIS 1.8.0–1.8.4 / 2.8.0–2.8.4) cache/dedup helper called
from `sub_77B4` — sets a 1-byte "suppress" flag at `unk_20000D68+0x10`
(import) / `+0x11` (export) whenever a tariff register present in the
PREVIOUS telegram is missing from the current one, within a 30-minute
window. This is an intentional "don't trust a register that just
vanished" dedup signal, and `sub_9DA0` already correctly implements it AT
THE VALUE LEVEL: it forces the corresponding value to the `0x7FFFFFFF`
sentinel when this fires, and the downstream reporting function
`sub_9D34` already no-ops cleanly on that sentinel. Per-field suppression
is therefore already fully and correctly handled.

The actual bug is one level up, in the outer dispatcher `sub_AA7C`
(`0xAAEE`–`0xAAFD`): it ALSO reads those same two flags and, if EITHER is
set, discards the ENTIRE decoded record by zeroing the struct pointer —
including the power value (decoded independently by unrelated function
`sub_9ED8`), whichever of import/export ISN'T flagged, and the
"processed OK" status write. This is redundant over-reach on top of logic
that already worked correctly one level down, and disproportionately
damaging for a meter like the MT-691 that reports only ONE tariff
register (`1-0:1.8.0`, no `1.8.1`–`1.8.4`): a single transient IR/parse
miss of that one register sets the flag and blanks EVERYTHING — power
included — for up to 30 minutes, even though the SAME telegram's power
field decoded fine.

**Fix**: delete the veto block. Pure removal, no replacement logic
needed, since suppression already happens correctly downstream in
`sub_9D34`. Confirmed via `xrefs_to` that nothing branches into the
middle of this 16-byte block, so it's safe to remove as a unit — but a
**second verification pass caught that plain NOPs would have been wrong**:
`0xAAFE` (right after the block) isn't padding, it's the live start of a
DIFFERENT switch case (reached via a separate `BEQ` earlier in the
function, calling `sub_7434` for a different code path). Falling through
into that via plain NOPs would have unconditionally run the wrong case
and clobbered the result register on every single telegram — a new,
worse bug than the one being fixed. The correct patch (applied) is an
explicit `B` to the true merge point `0xAB14`, followed by NOP filler,
overwriting the same 16 bytes (`0xAAEE`–`0xAAFD`). Does not touch
`sub_9DA0`/`sub_B348`/`sub_C310` at all, so the 30-minute dedup/cache
mechanism for genuine multi-tariff meters is fully preserved.

Since this variant's power reading (`-417 W`, Integer16) would ALSO have
hit the separate Integer16 sign-extension bug (see above) even after this
discard-block fix, `splice_ISKRA_MT691.py` includes BOTH fixes —
`entry_sxth16_fix` and the `sub_AA7C` patch — reusing the plain variant's
`hooks.bin` (no `FIX_NEGATIVE_POWER`-guarded hooks are needed here, unlike
the DWSB20.2TH variant).

**Confidence**: high for the branch-offset arithmetic specifically (an
independent second IDA pass caught the plain-NOP bug above, and the final
`B`-instruction offset was independently recomputed by hand outside of
IDA and matched exactly, plus cross-checked against an already-present
`B` instruction elsewhere in the same binary to validate the encoding
convention). Medium-high for the discard-flag root-cause analysis (traced
through real disassembly/decompile at every step, not inferred from
names) — the residual uncertainty is purely "does this actually fix the
real hardware's behavior," which by definition cannot be confirmed without
a real MT-691. If you have one and flash this build, feedback (via a
GitHub issue) on whether it helped would be genuinely valuable.

## Why a glue stub instead of a direct BL into the C function?

The vendor firmware's patch sites often do NOT follow plain AAPCS (e.g. the
return value goes through a pointer in R4 instead of R0, or a shared
function epilogue uses `pop {r3-r7,pc}` instead of `bx lr`, or LR needs to
be pushed/popped around an extra call the original single instruction
never made). A short glue stub captures that cleanly, while the actual
hook logic stays plain, compiler-checked C. This keeps the hand-written
assembly minimal and confined to one spot per hook.
