# 03 · Reverse Engineering

Protocol-level documentation recovered from the ESP32-C3 bridge firmware (RISC-V) and the BAT32G135
reader firmware (ARM Cortex-M0+).

## Pages
| File | What |
|---|---|
| [firmware-layout.md](firmware-layout.md) | Image structure, segments, embedded reader fw, vsocket/dispatch, OTA, boot hardening |
| [ble-protocol.md](ble-protocol.md) | GATT ABF0/1/2, TEA cipher, fragmentation, JSON commands |
| [lora-protocol.md](lora-protocol.md) | 868 MHz frame format, command set, directions, XOR, ECDH gate, energy payloads |
| [uart-config-protocol.md](uart-config-protocol.md) | `C5 5C` plaintext config channel (TEA key / WiFi read+write) |
| [cloud-api.md](cloud-api.md) | The vendor cloud REST API (auth, environments, endpoints, where the key/certs come from) |
| [security-notes.md](security-notes.md) | Consolidated weaknesses incl. the BLE reassembly heap overflow (partial) |

## Method (for reproducing)
The ESP-IDF bridge image was wrapped into a RISC-V ELF (segments mapped to their load addresses) and
loaded into IDA; ~1000 functions were named starting from the `__func__` strings ESP-IDF embeds for its
logger, then subsystem by subsystem. The reader image is a raw Cortex-M0+ binary appended after the ESP
segments — see [../firmware/README.md](../firmware/README.md) for the IDA setup script and load steps
(no vendor binaries are shipped; bring your own dump).

## Key facts (the short version)
- **BLE** control channel: **classic TEA** (32 rounds, delta `0x9E3779B9`, ECB), one 16-byte key per
  device, JSON payload, 173-byte fragmentation. Key is provisioned, stored in NVS.
- **LoRa** link: framed, dispatched by a 6-bit command; "encryption" is a **single-byte XOR** whose key
  is the byte-sum of the cleartext 3-byte handle. An ECDH P-256 exchange gates data flow but its shared
  secret is unused.
- **UART0** exposes a **plaintext** config protocol that can read/write the TEA key, WiFi credentials and
  more — the practical entry point for self-hosting.
- **OTA**: bridge self-update via MQTT (esp_ota, dual partition); reader firmware is **bundled inside the
  bridge image** and served to readers over LoRa.
- **Boot hardening**: eFuses disable JTAG + ROM download mode (locked bootloader).

All key material shown in examples is a placeholder.
