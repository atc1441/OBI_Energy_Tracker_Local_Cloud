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
  -Wall -Wextra -Werror -std=c11"

"$GCC" $CFLAGS -c hooks.c -o build/hooks.o
"$GCC" -mcpu=cortex-m0plus -mthumb -c entry.S -o build/entry.o

"$GCC" $CFLAGS -nostdlib -Wl,-T,link.ld -Wl,--gc-sections \
  -Wl,-Map=build/hooks.map -o build/hooks.elf build/hooks.o build/entry.o

"$OBJCOPY" -O binary build/hooks.elf build/hooks.bin

# symbol table: "ADDR T name" lines only, for splice.py
"$NM" --defined-only build/hooks.elf | awk '$2=="T" || $2=="t" {print}' > build/hooks.sym

echo "--- hooks.sym ---"
cat build/hooks.sym
echo "--- size ---"
"$SIZE" build/hooks.elf
ls -la build/hooks.bin
