#!/usr/bin/env bash
# build.sh — compile+link the C hooks for the BAT32G135 reader firmware
# (Cortex-M0+ / ARMv6-M, Thumb-1 only, no FPU) and extract a raw binary
# blob plus a symbol map for splice.py. Source (hooks.c/entry.S/link.ld)
# lives here; every compiled artifact goes into build/, next to nothing
# else.
#
# Toolchain lookup: prefers a plain `arm-none-eabi-gcc` on PATH (true on
# Linux CI after `apt-get install gcc-arm-none-eabi`), falls back to the
# PlatformIO-bundled Windows toolchain for local dev on this machine.
#
# Variant: pass "DWSB20_2TH" as $1 to additionally build the DWSB20.2TH
# negative-power fix into hook_decode_int24 (-DFIX_NEGATIVE_POWER),
# writing to build/hooks_DWSB20_2TH.{o,elf,bin,sym} instead of the plain
# build/hooks.* -- so both variants can be built back-to-back without one
# overwriting the other (splice.py / splice_DWSB20_2TH.py each read their
# own).
VARIANT="${1:-plain}"
if [ "$VARIANT" = "DWSB20_2TH" ]; then
  DEFS="-DFIX_NEGATIVE_POWER"
  OUT="build/hooks_DWSB20_2TH"
else
  DEFS=""
  OUT="build/hooks"
fi

set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  GCC=arm-none-eabi-gcc
  OBJCOPY=arm-none-eabi-objcopy
  NM=arm-none-eabi-nm
  SIZE=arm-none-eabi-size
else
  TC="$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin"
  GCC="$TC/arm-none-eabi-gcc.exe"
  OBJCOPY="$TC/arm-none-eabi-objcopy.exe"
  NM="$TC/arm-none-eabi-nm.exe"
  SIZE="$TC/arm-none-eabi-size.exe"
fi

CFLAGS="-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft -Os \
  -ffreestanding -fno-builtin -fomit-frame-pointer \
  -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -Wall -Wextra -Werror -std=c11 $DEFS"

"$GCC" $CFLAGS -c hooks.c -o "$OUT.o"
"$GCC" -mcpu=cortex-m0plus -mthumb $DEFS -c entry.S -o "$OUT-entry.o"

"$GCC" $CFLAGS -nostdlib -Wl,-T,link.ld -Wl,--gc-sections \
  -Wl,-Map="$OUT.map" -o "$OUT.elf" "$OUT.o" "$OUT-entry.o"

"$OBJCOPY" -O binary "$OUT.elf" "$OUT.bin"

# symbol table: "ADDR T name" lines only, for splice.py
"$NM" --defined-only "$OUT.elf" | awk '$2=="T" || $2=="t" {print}' > "$OUT.sym"

echo "--- $OUT.sym ---"
cat "$OUT.sym"
echo "--- size ---"
"$SIZE" "$OUT.elf"
ls -la "$OUT.bin"
