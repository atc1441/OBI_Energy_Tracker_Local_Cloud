# Status, firmware coverage & roadmap

## TL;DR — what works today ✅
The project is in a **healthy, working state**. The two headline goals are done and verified on real hardware:

- **✅ Open ESP32-C3 gateway firmware is complete and runs in daily use** — it fully replaces the vendor
  bridge. Pairs the LoRa readers itself, decrypts the energy, and serves a local **web dashboard (DE/EN)**,
  **MQTT + MQTTS (TLS)** with Home-Assistant auto-discovery, a session **login**, per-reader energy
  **history** (daily kWh + cost), **reader-OTA over LoRa**, gateway **self-OTA** (web upload / GitHub
  release), a web **factory reset**, and **RSSI + SNR** per reader. Builds cleanly for **6 board targets**
  (stock OBI C3, Heltec Vision Master E290, LILYGO T-Beam, Seeed XIAO S3, generic ESP32 / ESP32-S3).
  → [`open_obi_energy_meter/`](open_obi_energy_meter/).
- **✅ Own-cloud path for the *stock* bridge is complete** — TEA key → PKI → MQTTS broker → BLE
  provisioning → live telemetry, and the **custom-firmware OTA** that installs the firmware above. All
  verified on hardware.
- **✅ Energy telemetry is fully decoded** and confirmed on a live device.

Everything below is the deeper **reverse-engineering coverage** of the vendor system. The pieces that
matter — telemetry **and** the downlink command path — are now **fully reversed and verified**; and in any
case none of it is a limitation of the open firmware, which already does pairing, energy, config and OTA
locally.

## Which firmware this repo covers
- **Bridge (ESP32-C3):** analyzed across **1.0.0 – 1.2.1** and the separate **31.0.0 – 34.0.0** track.
- **Protocol details and function addresses are from `1.0.2`** (the primary reversed image). Findings were
  spot-checked and hold across the other versions (same TEA, UART config protocol, LoRa framing, and
  fleet provisioning).
- **Reader (BAT32G135):** extractable from every bridge build; findings are from the `31.0.0` reader. (The
  `1.2.x` reader is a larger build — see [02-hardware](02-hardware/README.md).)
- 🔒 **No firmware binaries are shipped in this repo** (SUMEC/OBI copyright, removed for safety and
  git-ignored). Bring your own dump — see [firmware/README.md](firmware/README.md).

> ⚠️ **Firmware is a moving target.** A future OTA can change UUIDs, command ids, payload layouts, timing,
> or add real crypto. Everything here reflects the versions above — re-verify against your own unit before
> relying on a detail.

## OTA signature status (as of the newest analyzed version)
- The bridge self-update (MQTT OTA) carries only an **integrity hash (16 bytes, MD5-style)** — **not a
  cryptographic signature**. No secure-boot signature enforcement was found, and the app image has no
  flash-encryption strings.
- **Consequence:** a firmware image delivered through **your own cloud** OTA path is accepted — which is
  exactly what makes **custom firmware** feasible despite the locked bootloader
  ([firmware-layout](03-reverse-engineering/firmware-layout.md#boot-hardening-locked-bootloader)).
- This could change: a future version may add **signed OTA / secure boot**, which would break the custom-
  firmware route.

## Reverse-engineering coverage — complete ✅
Both big pieces are now **done and verified on hardware**:

1. **Real energy data over MQTT.** ✅ The telemetry is JSON and decoded end-to-end (statically + confirmed
   on a live device). The meter readings ride
   `$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state` (and `dt/…/state/live`) as
   `{uuid, bridge_uuid, …, timestamp, rssi, battery, energy, negative_energy, power}`; the bridge/heartbeat
   state carries sensor status. Full schema + a live example:
   [cloud-api.md](03-reverse-engineering/cloud-api.md#telemetry-payloads-decoded--confirmed-on-a-live-device).
2. **Talking to the device over MQTT.** ✅ The downlink/command path (protocol 2 over pipe 0) is **fully
   reversed and verified**, and the available commands are implemented in the broker tool:
   - **Reader upload-interval change** — publish `{sensor_upload_interval, session_id}` to
     `cmd/…/sensor/<UUID>/upload-interval-change-request`; `mqtt_set_upload_interval` (proto 2 cmd 6)
     applies it and replies on `…-response`. Broker: `mqtts_server.py --set-interval N`.
   - **OTA firmware update** — the 23-byte offer + JSON data-request + 9-byte-header data-response chunk
     protocol is fully mapped (`mqtt_ota_file_info` / `mqtt_ota_data`), confirmed byte-for-byte against a
     real cloud capture and an end-to-end self-test. The self-update is **unsigned**, so this is a working
     **custom-firmware** path. Broker: `mqtts_server.py --ota-firmware fw.bin`. See
     [OTA](03-reverse-engineering/cloud-api.md#ota) / [flash firmware](04-connect-your-own-cloud/README.md#flash-your-own-firmware).

   All available downlink commands are documented under
   [downlink commands](03-reverse-engineering/cloud-api.md#downlink-commands-cloud--device).

**Known characteristic of the *stock bridge* (not a bug):** on the vendor firmware, adding/pairing a reader
is **BLE-only** — there is no MQTT/cloud command to scan or bind a sensor, and the bridge's BLE is only
active during a setup window. Re-open that window anytime by **holding the gateway's button for ~5 s**
(re-activates BLE for general config and adding sensors), then pair over BLE (built into
`ble_provision.py --pair-sensor`) — see [07 · Add a reader](07-add-a-reader/README.md).
> This is **gone on the open firmware**: it pairs readers itself over LoRa (**"Bind to gateway" / "Bind all
> for 3 min"** in the web dashboard), no BLE and no cloud involved. The exact SX1262 RF params are known too
> — reversed from reader `v1.2.1` and running in the open gateway: **869.5 MHz · LoRa · BW 500 kHz · SF7 ·
> CR 4/5 · preamble 12 · sync 0x1424 · +22 dBm · TCXO 1.8 V** (see
> [platformio.ini](open_obi_energy_meter/platformio.ini) / [lora-protocol.md](03-reverse-engineering/lora-protocol.md)).

Contributions are still welcome for extending the vendor-system RE further — starting points:
[own cloud + broker log](04-connect-your-own-cloud/README.md),
[MQTT topics](03-reverse-engineering/firmware-layout.md#mqtt-topics),
[LoRa energy payload](03-reverse-engineering/lora-protocol.md#energy-payload-cmd-19--22--23--24--25).
