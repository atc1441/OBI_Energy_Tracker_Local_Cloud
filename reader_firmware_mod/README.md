# reader_sdk — a C-hook framework for the BAT32G135 reader firmware

> ➡️ **Deutsche Version: [de/README.md](de/README.md)**

Goal: instead of hand-translating every change to the (closed-source) vendor
firmware into Thumb-1 opcodes, write the hook code in C, compile it with a
real `arm-none-eabi-gcc` (Cortex-M0+/ARMv6-M, Thumb-1, no FPU), and splice the
result into the firmware.

## Folder layout

- `reader_stock_v57.bin` — unmodified original firmware (the source
  `splice.py` patches).
- `reader_modded_v87.bin` — finished result: just the int24 fix (SML
  TL=0x54), softver set to 87, IDA-verified. This is the only file you
  actually need to flash.
- `hooks.c`, `entry.S`, `link.ld`, `vendor.h`, `build.sh`, `splice.py` —
  source and build scripts, live directly here.
- `build/` — ONLY compiled intermediate files (`hooks.o`, `entry.o`,
  `hooks.elf`, `hooks.map`, `hooks.sym`, `hooks.bin`). `build.sh` creates
  the folder itself if needed.

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
                    addresses for splice.py).
- `splice.py`       — appends hooks.bin to the firmware image and patches
                    the call sites (BL encoding computed programmatically,
                    same as before) to jump into the entry points from
                    entry.S.

## Adding a new hook

1. Write the function in `hooks.c` (plain C, no libc other than what
   `vendor.h` declares).
2. If the call site doesn't use plain AAPCS (e.g. a vendor-specific
   register convention): add a small trampoline stub in `entry.S` that
   moves the registers into AAPCS arguments (R0,R1,R2,R3), `bl`s into the
   C function, then performs whatever return sequence the call site
   expects (e.g. `pop {r3-r7,pc}`).
3. Add the call-site address + the entry.S symbol name to `HOOKS = [...]`
   in `splice.py`.
4. `./build.sh && python splice.py` — produces a new, patched `.bin`.
5. **Always** cross-check with IDA (`idalib_open` + `disasm`) before
   flashing — splicing itself only validates byte encoding, not semantics.

## Why a glue stub instead of a direct BL into the C function?

The vendor firmware's patch sites often do NOT follow plain AAPCS (e.g. the
return value goes through a pointer in R4 instead of R0, or a shared
function epilogue uses `pop {r3-r7,pc}` instead of `bx lr`). A short glue
stub captures that cleanly, while the actual hook logic stays plain,
compiler-checked C. This keeps the hand-written assembly minimal and
confined to one spot per hook.
