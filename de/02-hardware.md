# 02 · Hardware (🇩🇪)

Zwei MCUs, eine Radio‑Familie. Alles unten stammt aus der Firmware und dem BAT32G135‑Datenblatt.

## Gateway — ESP32‑C3
- **Core:** RISC‑V RV32IMC, single core.
- **Radio:** Semtech **SX1262** auf einem **Ai‑Thinker Ra‑03SCH**‑Modul (auch als LLCC68/SX126x‑Familie —
  command‑kompatibel), 868 MHz.
- **Konsole:** UART0 mit **115200 8N1** (Default‑C3‑Pins **GPIO20 = RX, GPIO21 = TX**). Das ist zugleich der
  Klartext‑**Config‑Kanal** — siehe [03-uart-config.md](03-uart-config.md).
- **Security‑Fuses:** beim Boot brennt die Firmware eFuses, die **JTAG und den ROM‑Download‑Modus
  deaktivieren** (gesperrter Bootloader), falls der USER_DATA‑Fuse gesetzt ist. Folge: `esptool
  read_flash`/Reflash über UART ist auf Produktivgeräten blockiert.

### SPI zum Radio (aus `mdrvspi`)
| Signal | GPIO |
|---|---|
| SPI‑Host | SPI2 |
| MOSI | GPIO7 |
| MISO | GPIO2 |
| SCLK | GPIO6 |
| NSS / CS | GPIO10 (manuell) |
| Takt | 8 MHz |

Der SX126x‑Treiber nutzt außerdem **DIO2 als RF‑Switch** und **DIO3 als TCXO**‑Steuerung und pollt den
**BUSY**‑Pin (typisch SX126x).

## Reader — BAT32G135
**MCU** von Singapore Changi Technology / CMSemicon (中微半导体); Datenblatt
`BAT32G135_datasheet_V1.40.pdf` (Vendor‑Tools: BATCube, CMS WriterPro). Das **Produkt/den Reader** stellt
**SUMEC** (<https://en.sumec.com/>) her, verkauft unter **heyOBI** — Firmware‑Strings und Cloud‑IDs tragen
den `SUMEC`‑Namen.

| Eigenschaft | Wert |
|---|---|
| Core | ARM **Cortex‑M0+** (ARMv6‑M, Thumb‑only), bis 64 MHz |
| Code‑Flash | **64 KB** @ `0x0000_0000`–`0x0000_FFFF` |
| Special Data‑Flash | 1.5 KB @ `0x0050_0000` |
| SRAM | **8 KB** (Parity) @ `0x2000_0000`–`0x2000_1FFF` |
| Peripherie | `0x4000_0000`–`0x4005_FFFF` |
| Cortex‑M SCS | `0xE000_0000` |
| App | SUMEC‑Stromzähler‑Reader (OBIS/IEC‑62056), optische/IR‑Schnittstelle |

> Hinweis: die Reader‑Images aus Gateway **1.2.x** sind ~78 KB (über 64 KB) — die zielen auf eine
> Flash‑größere BAT32‑Variante / neuere Reader‑HW. Die ~44‑KB‑Images (1.0.x, 3x.0.0) passen exakt in den
> BAT32G135.

IDA‑fertige Extraktion & Ladeanleitung: [../firmware/README.md](../firmware/README.md).

## Radio‑Parameter (SX1262)
- **Band:** 868 MHz. **Modulation:** LoRa. SF/BW/CR/Preamble setzt die App zur Laufzeit über die Semtech‑
  `Radio`‑API (nicht als Konstanten). BW‑Index 0 = 125 kHz (Default). Für die exakten Werte den SPI‑Bus
  beim Boot mitschneiden und die Kommandos `0x86` (SetRfFrequency, 4 FRF‑Bytes → `freq = FRF·32 MHz / 2²⁵`)
  und `0x8B` (SetModulationParams: SF, BW, CR) dekodieren.

## Zugänge zum Basteln
- **UART0 (GPIO20/21, 115200 8N1):** Geräte‑Log + Config‑Protokoll (TEA‑Key/WLAN lesen/schreiben, LoRa‑Test).
- **SPI2 (GPIO6/7/2/10):** SX1262 sniffen für die exakten RF‑Parameter.
- **BLE (ABF0/1/2):** der App‑Steuerkanal (TEA + JSON).
- **868‑MHz‑Funk:** der Reader↔Gateway‑Link (siehe [05-lora-direkt.md](05-lora-direkt.md)).
