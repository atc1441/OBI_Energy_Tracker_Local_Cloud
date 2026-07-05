# Firmware images — not included

> **For safety, no vendor firmware binaries are distributed in this repository.** The bridge (ESP32-C3)
> and reader (BAT32G135) images are **SUMEC / OBI copyright**, so they are removed and git-ignored
> (`firmware/**/*.bin`, `*.elf`). This repo ships only *documentation and tools* — you supply the image
> from a device **you own**.

## How to get an image (yours)
- **Pull it from your own cloud account** with
  [`../04-connect-your-own-cloud/tools/obi_ota_download.py`](../04-connect-your-own-cloud/tools/obi_ota_download.py):
  it replays the device's OTA client and saves the app image the vendor serves for *your* bridge
  (needs a provisioning cert for your bridge's UUID — see
  [../03-reverse-engineering/cloud-api.md](../03-reverse-engineering/cloud-api.md)).
- **Dump it from your own device once the custom firmware runs**: the web debug page reads the whole SPI
  flash (both OTA slots → running *and* previous version). No cert or esptool needed — steps + a
  partition splitter in
  [../04-connect-your-own-cloud/dump-your-flash.md](../04-connect-your-own-cloud/dump-your-flash.md).
- Or use a dump you already have. The ROM download mode is fused off on shipped units
  (locked bootloader — see [../03-reverse-engineering/firmware-layout.md](../03-reverse-engineering/firmware-layout.md)),
  so UART/JTAG readout is not available; the OTA path above is the practical route.

No device-specific credentials live in the app image anyway — the TEA key and cloud certs are in the
NVS/data partitions at runtime, not in the code image.

## Loading the ESP32-C3 bridge image (RISC-V)
Map each segment to its load address (a flat load will not disassemble). The wrapper approach and segment
map are in [../03-reverse-engineering/firmware-layout.md](../03-reverse-engineering/firmware-layout.md).

## Loading the BAT32G135 reader image (ARM Cortex-M0+)
`reader/cortexm_setup.py` is included — an IDA script that adds the exact BAT32G135 memory map and names the
vector table. Steps once you have your own `reader.bin`/`reader.elf`:

1. Load the image in IDA as **ARM Little-endian, Thumb (Cortex-M0+)**, base `0x0` (an ELF wrapper
   auto-detects; a raw `.bin` needs processor **ARMv6-M / Cortex-M0+** set manually).
2. Run `reader/cortexm_setup.py` (File → Script file) to add `DATAFLASH`/`SRAM`/`PERIPH`/`SCS` segments,
   force Thumb, and name the exception/IRQ vectors.

Memory map (from the datasheet): code flash 64 KB @`0x0`, data flash 1.5 KB @`0x500000`, SRAM 8 KB
@`0x20000000`, peripherals `0x40000000`–`0x4005FFFF`, SCS `0xE0000000`. The reader is a SUMEC
electricity-meter reader (OBIS/IEC-62056) — see [../02-hardware/README.md](../02-hardware/README.md).
Different bridge versions carry different reader builds; `1.2.x` readers are larger (bigger-flash BAT32
variant).
