// board_config.h — pick your board (or define OBI_BOARD_CUSTOM and set the pins).
// Selection is by a -D build flag in platformio.ini; the file is otherwise board-agnostic.
//
// Any ESP32 + Semtech SX1262 works. You only need to know 7 GPIOs (NSS/SCK/MOSI/MISO/
// RST/BUSY/DIO1) plus the TCXO voltage (0 = module has a plain crystal, not a TCXO).
#pragma once

#if defined(OBI_BOARD_HELTEC_S3)          // Heltec Vision Master E290 / T190, WiFi LoRa 32 V3, Wireless Stick V3
  #define PIN_LORA_NSS   8
  #define PIN_LORA_SCK   9
  #define PIN_LORA_MOSI  10
  #define PIN_LORA_MISO  11
  #define PIN_LORA_RST   12
  #define PIN_LORA_BUSY  13
  #define PIN_LORA_DIO1  14
  #define LORA_TCXO_V    1.8f     // Heltec SX1262 has a TCXO on DIO3 @ 1.8 V
  #define LORA_RXEN_PIN  RADIOLIB_NC
  #define LORA_DIO2_RFSW true     // RF switch is driven from DIO2
  // On-board 2.9" e-paper (DEPG0290BNS800 / SSD1680, 296x128) on its OWN pins/SPI bus
  // (separate from LoRa), so it never touches the radio timing. Verified against the
  // Heltec Vision Master E290 pin map / Meshtastic variant.
  #define OBI_HAS_EPD    1
  #define PIN_EINK_CS    3
  #define PIN_EINK_DC    4
  #define PIN_EINK_RST   5
  #define PIN_EINK_BUSY  6
  #define PIN_EINK_SCLK  2
  #define PIN_EINK_MOSI  1
  #define PIN_EINK_VEXT  18       // display power enable — active HIGH
  // User button (GPIO21) — same behaviour as the OBI gateway's case button: hold >=5 s to factory-reset
  // (wipe WiFi/MQTT + reader list) and reboot into the setup portal. Active-low (INPUT_PULLUP).
  #define PIN_BUTTON        21
  #define BUTTON_ACTIVE_LOW 1

#elif defined(OBI_BOARD_TTGO_TBEAM_SX1262) // LILYGO T-Beam / T3 with SX1262
  #define PIN_LORA_NSS   18
  #define PIN_LORA_SCK   5
  #define PIN_LORA_MOSI  27
  #define PIN_LORA_MISO  19
  #define PIN_LORA_RST   23
  #define PIN_LORA_BUSY  32
  #define PIN_LORA_DIO1  33
  #define LORA_TCXO_V    1.8f
  #define LORA_RXEN_PIN  RADIOLIB_NC
  #define LORA_DIO2_RFSW true

#elif defined(OBI_BOARD_OBI_C3)            // The ORIGINAL OBI/heyOBI gateway (ESP32-C3 + SX1262 on an
  // Ai-Thinker Ra-03SCH). Pins reversed from stock firmware 1.2.1 (see 02-hardware/README.md and the
  // decompiled mdrvspi/SX126x board init). This build is meant to be delivered via the cloud MQTT OTA
  // path — the stock bootloader is locked (DIS_JTAG + DIS_DOWNLOAD_MODE), so you cannot flash over UART.
  #define PIN_LORA_NSS   10       // SPI CS, driven manually  (sub_4201A076)
  #define PIN_LORA_SCK   6
  #define PIN_LORA_MOSI  7
  #define PIN_LORA_MISO  2
  #define PIN_LORA_RST   18       // NRESET               (sub_4201A3C2 reset toggle)
  #define PIN_LORA_BUSY  1        // SX126x BUSY          (sub_4201A33E, input+pullup)
  #define PIN_LORA_DIO1  19       // IRQ line             (sub_4201A362, gpio ISR)
  #define LORA_TCXO_V    1.8f     // SX1262 TCXO on DIO3 @ 1.8 V
  #define LORA_RXEN_PIN  RADIOLIB_NC
  #define LORA_DIO2_RFSW true     // RF switch driven from DIO2
  // Case button (sub_4201A48C: GPIO8, plain input). GPIO8 is a C3 strapping pin held high by an external
  // pull-up, so the button pulls it to GND -> pressed reads LOW. Held >=5 s -> factory reset (see main.cpp).
  #define PIN_BUTTON        8
  #define BUTTON_ACTIVE_LOW 1
  #define PIN_STATUS_LED    0     // on-board LED (sub_4201A04E/03A, active-low); optional reset feedback
  #define STATUS_LED_ACTIVE_LOW 1

#elif defined(OBI_BOARD_XIAO_ESP32S3)       // Seeed Studio XIAO ESP32S3 & Wio-SX1262 Kit (B2B)
  // Pin mapping via the on-board B2B connector. Wio-SX1262 has a TCXO on DIO3,
  // DIO2 drives the internal RF switch, and RF_SW (GPIO38) is the RX enable.
  #define PIN_LORA_NSS   41
  #define PIN_LORA_SCK   7
  #define PIN_LORA_MOSI  9
  #define PIN_LORA_MISO  8
  #define PIN_LORA_RST   42
  #define PIN_LORA_BUSY  40
  #define PIN_LORA_DIO1  39
  #define LORA_TCXO_V    1.8f     // Wio-SX1262 TCXO on DIO3
  #define LORA_RXEN_PIN  38       // RF_SW on module — HIGH during RX
  #define LORA_DIO2_RFSW true

#elif defined(OBI_BOARD_CUSTOM)            // <-- your own wiring: edit these
  #define PIN_LORA_NSS   8
  #define PIN_LORA_SCK   9
  #define PIN_LORA_MOSI  10
  #define PIN_LORA_MISO  11
  #define PIN_LORA_RST   12
  #define PIN_LORA_BUSY  13
  #define PIN_LORA_DIO1  14
  #define LORA_TCXO_V    1.8f     // set 0.0f if your SX1262 module uses a crystal (XTAL), not a TCXO
  #define LORA_RXEN_PIN  RADIOLIB_NC   // set a GPIO if your module has a separate RXEN line
  #define LORA_DIO2_RFSW true

#else
  #error "No board selected. Add -D OBI_BOARD_HELTEC_S3 (or _TTGO_TBEAM_SX1262 / _OBI_C3 / _XIAO_ESP32S3 / _CUSTOM) to build_flags in platformio.ini"
#endif
