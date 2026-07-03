# Firmware layout & internals

## ESP32-C3 bridge image
Standard ESP-IDF application image (magic `0xE9`, chip id 5 = ESP32-C3), 6 segments:

| Seg | Load addr | Region |
|---|---|---|
| DROM | `0x3C120020` | const / rodata (strings) |
| DRAM | `0x3FC93000` | .data |
| IRAM | `0x40380000`, `0x403880A0` | code |
| IROM | `0x42000020` | main `.text` (~1.2 MB) |
| RTC | `0x50000010` | rtc |

**Appended after the segments (past the image SHA-256): the reader firmware** — a raw ARM Cortex-M0+
image (BAT32G135). This is the `filec`-marked blob the bridge serves to readers over LoRa OTA. Extract
it and load in IDA via [../firmware/README.md](../firmware/README.md).

To load the bridge itself in IDA/Ghidra you must map each segment to its load address (a flat load
disassembles to garbage). Easiest: wrap the segments in a RISC-V ELF with one PT_LOAD per segment.

## Internal dispatch (vsocket)
One framing layer multiplexed by a **pipe id**, dispatched by a **protocol** via a `(proto<<16)|cmd`
handler table (`knock` registers, `cmd_dispatch` invokes):

| pipe | fed by | protocol | commands |
|---|---|---|---|
| 1 | BLE ABF2 | 1 | BLE JSON (Status/WifiSet/…/Unbind = 1..8) |
| 0 | MQTT / BLE pipe0 | 2 | management + **OTA-from-cloud** (file-info=7, data=8) |
| 2 | **UART0** | 254 | plaintext config (48/49/52/55/58/59) |
| (radio) | LoRa | 0 | reader commands (17/19/22/…/32) |

The pipe id is fixed by the transport, so e.g. the UART-only config space (254) can't be reached over
BLE, and BLE always lands on the TEA-protected JSON space (1).

## OTA — two mechanisms
1. **Bridge self-update (ESP32-C3)** over MQTT (proto 2): a 23-byte `firmware-update-request` offer
   `[3B version][u32 total_len BE][16B md5]` → `esp_ota_begin` on the *next* OTA partition; the device
   then pulls `firmware-data-request {offset}` and gets `firmware-data-response` chunks
   `[3B version][u32 offset BE][u16 len BE][data≤512]` → `esp_ota_write`; at 100% → set boot partition +
   reboot. Pull-based; topics `…/ota/firmware-update-*` and `…/ota/firmware-data-*`. **Unsigned** (md5 only)
   — full protocol + the push tool in [cloud-api.md#ota](cloud-api.md#ota). Verified live (1.0.1 → 1.2.1).
2. **Reader firmware relay (BAT32G135)** over LoRa: on 1.0.x the reader image is bundled in the bridge's
   app partition and served read-only (LoRa cmd 20/21). On **1.2.x** the bridge instead **fetches the
   reader image from the cloud** over the same `firmware-data-request` channel (a separate `offset` stream)
   and then flashes the reader over LoRa (`upgradeserver … upgrade success`; observed reader `32.0.0 →
   57.0.0`).

## Boot hardening (locked bootloader)
At boot the firmware runs a hardening step (conditional on the USER_DATA eFuse being provisioned):
- burns **DIS_JTAG**,
- burns **DIS_DOWNLOAD_MODE**.

Both are one-way. On a provisioned unit this means **no `esptool` flash read/write over UART** and no
JTAG. Consequence for custom firmware: you cannot flash directly — a modified image must be delivered
through the **cloud OTA path** (mechanism 1 above), which is why self-hosting the cloud is the route to
custom firmware. No flash-encryption strings are present in the app image.

## MQTT topics (structure, placeholders for ids)
```
cmd|dt /energy-tracking/bridge/<UUID>/sensor/<UUID>/state/live[-ack]
cmd|dt /energy-tracking/bridge/<UUID>/sensor/<UUID>/upload-interval-change-*
cmd|dt /energy-tracking/bridge/<UUID>/ota/firmware-update-*  ·  firmware-data-*
$aws/certificates/create/json[/accepted]
$aws/provisioning-templates/<template>/provision/json[/accepted]
$aws/rules/EnergyTrackingBridge[Heartbeat]/<UUID>/state
$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state
```
