#!/usr/bin/env bash
# build.sh — assemble the legacy v32 live hook into build/live.bin
# (plus build/live.sym for patch_legacy_live.py).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  P=arm-none-eabi-
else
  P="$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-"
fi

"${P}gcc" -mcpu=cortex-m0plus -mthumb -c live.S -o build/live.o
"${P}gcc" -mcpu=cortex-m0plus -mthumb -nostdlib -Wl,-T,link.ld \
  -o build/live.elf build/live.o
"${P}objcopy" -O binary -j .text build/live.elf build/live.bin
"${P}nm" --defined-only build/live.elf | awk '$2=="T" || $2=="t" {print}' > build/live.sym

cat build/live.sym
ls -la build/live.bin
