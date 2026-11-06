#!/usr/bin/env bash
# build.sh — assemble the position-independent v57 live hooks
# into build/live.bin (+ build/live.sym offsets) for patch_live.py.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  P=arm-none-eabi-
else
  P="$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-"
fi

"${P}gcc" -mcpu=cortex-m0plus -mthumb -c live.S -o build/live.o
if "${P}readelf" -r build/live.o | grep -q "Relocation section"; then
  echo "live.S must stay position independent" >&2; exit 1
fi
"${P}objcopy" -O binary -j .text.live build/live.o build/live.bin
"${P}nm" --defined-only build/live.o | awk '$2=="T" {print}' > build/live.sym

cat build/live.sym
ls -la build/live.bin
