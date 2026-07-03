# 02 · Hardware

Two MCUs, one radio family. Everything below is derived from the firmware and the BAT32G135 datasheet.

## Bridge — ESP32-C3
- **Core:** RISC-V RV32IMC, single core.
- **Radio:** Semtech **SX1262** on an **Ai-Thinker Ra-03SCH** module (also seen as LLCC68/SX126x family —
  command-compatible), 868 MHz.
- **Console:** UART0 at **115200 8N1** (default C3 console pins **GPIO20 = RX, GPIO21 = TX**). This is
  also the plaintext **config channel** — see [../03-reverse-engineering/uart-config-protocol.md](../03-reverse-engineering/uart-config-protocol.md).
- **Security fuses:** at boot the firmware burns eFuses to **disable JTAG and the ROM download mode**
  (locked bootloader) if the USER_DATA fuse block is provisioned. Consequence: `esptool read_flash`/
  reflash over UART is blocked on production units.

### SPI to the radio (from `mdrvspi`)
| Signal | GPIO |
|---|---|
| SPI host | SPI2 |
| MOSI | GPIO7 |
| MISO | GPIO2 |
| SCLK | GPIO6 |
| NSS / CS | GPIO10 (driven manually) |
| Clock | 8 MHz |

The SX126x driver also uses **DIO2 as RF switch** and **DIO3 as TCXO** control, and polls the **BUSY**
pin (a Semtech SX126x hallmark).

## Reader — BAT32G135
**MCU** by Singapore Changi Technology / CMSemicon (中微半导体); datasheet `BAT32G135_datasheet_V1.40.pdf`
(vendor tools: BATCube, CMS WriterPro). The **product/reader** itself is manufactured by **SUMEC**
(<https://en.sumec.com/>) and sold under the **heyOBI** brand — the reader firmware strings and cloud
identifiers carry the `SUMEC` name.

| Property | Value |
|---|---|
| Core | ARM **Cortex-M0+** (ARMv6-M, Thumb-only), up to 64 MHz |
| Code flash | **64 KB** @ `0x0000_0000`–`0x0000_FFFF` |
| Special data flash | 1.5 KB @ `0x0050_0000` |
| SRAM | **8 KB** (parity) @ `0x2000_0000`–`0x2000_1FFF` |
| Peripherals | `0x4000_0000`–`0x4005_FFFF` |
| Cortex-M SCS | `0xE000_0000` |
| App | SUMEC electricity-meter reader (OBIS/IEC-62056), optical/IR meter interface |

> Note: the reader image carried by bridge **1.2.x** is ~78 KB (exceeds 64 KB) — it targets a larger-flash
> BAT32 variant / newer reader HW. The ~44 KB images (1.0.x, 3x.0.0) fit BAT32G135 exactly.

Extraction and IDA load instructions (bring your own dump — no vendor binaries are shipped):
[../firmware/README.md](../firmware/README.md).

## Radio parameters (SX1262)
- **Band:** 868 MHz (Ra-03SCH is the sub-GHz SX126x variant; 410–525/863–928 depending on module build).
- **Modulation:** LoRa. SF/BW/CR/preamble are set at runtime by the app via the Semtech `Radio` API
  (`RadioSetTxConfig`/`RadioSetChannel`), not baked in as constants. Bandwidth index 0 = 125 kHz is the
  default (Semtech `Bandwidths[]` order 125/250/500…). To capture the exact values, probe the SPI bus at
  boot and decode the `0x86` (SetRfFrequency, 4 FRF bytes → `freq = FRF·32 MHz / 2²⁵`) and `0x8B`
  (SetModulationParams: SF, BW, CR) commands.

## Access points for tinkering
- **UART0 (GPIO20/21, 115200 8N1):** device log + config protocol (read/write TEA key, WiFi, LoRa test).
- **SPI2 (GPIO6/7/2/10):** sniff the SX1262 to recover exact RF params.
- **BLE (ABF0/1/2):** the app-facing control channel (TEA + JSON).
- **868 MHz air:** the reader↔bridge link (see [../05-lora-direct-868mhz](../05-lora-direct-868mhz/)).
