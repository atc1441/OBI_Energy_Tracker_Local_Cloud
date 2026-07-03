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
  #error "No board selected. Add -D OBI_BOARD_HELTEC_S3 (or _CUSTOM) to build_flags in platformio.ini"
#endif
