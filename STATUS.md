# Status, firmware coverage & roadmap

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

## Still missing / help wanted 🚧
Two big pieces are **not done yet** — this is the main open work:

1. ~~**Real energy data over MQTT.**~~ ✅ **Done** — the telemetry is JSON and decoded end-to-end
   (statically + confirmed on a live device). The meter readings ride
   `$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state` (and `dt/…/state/live`) as
   `{uuid, bridge_uuid, …, timestamp, rssi, battery, energy, negative_energy, power}`; the bridge/heartbeat
   state carries sensor status. Full schema + a live example:
   [cloud-api.md](03-reverse-engineering/cloud-api.md#telemetry-payloads-decoded--confirmed-on-a-live-device).
   Only remaining: pin the **unit** of `energy` against a physical meter's 1.8.0 display.
2. **Talking to the device over MQTT.** *In progress — two downlink commands fully reversed & verified.*
   The downlink/command path (protocol 2 over pipe 0) now has:
   - **Reader upload-interval change** — publish `{sensor_upload_interval, session_id}` to
     `cmd/…/sensor/<UUID>/upload-interval-change-request`; `mqtt_set_upload_interval` (proto 2 cmd 6)
     applies it and replies on `…-response`. Broker: `mqtts_server.py --set-interval N`.
   - **OTA firmware update** — the 23-byte offer + JSON data-request + 9-byte-header data-response chunk
     protocol is fully mapped (`mqtt_ota_file_info` / `mqtt_ota_data`), confirmed byte-for-byte against a
     real cloud capture and an end-to-end self-test. The self-update is **unsigned**, so this is a working
     **custom-firmware** path. Broker: `mqtts_server.py --ota-firmware fw.bin`. See
     [OTA](03-reverse-engineering/cloud-api.md#ota) / [flash firmware](04-connect-your-own-cloud/README.md#flash-your-own-firmware).

   Both are documented under [downlink commands](03-reverse-engineering/cloud-api.md#downlink-commands-cloud--device).
   Still open: the remaining command payloads (general status/config/control).

**Known constraint (not a bug to fix):** adding/pairing a reader is **BLE-only** — there is no MQTT/cloud
command to scan or bind a sensor, and the bridge's BLE is only active during a setup window. So pairing has
to happen in the BLE setup session (built into `ble_provision.py --pair-sensor`) — see
[07 · Add a reader](07-add-a-reader/README.md).

Also open (smaller): confirm the multi-reader `SensorScan` enumeration ([07](07-add-a-reader/README.md)),
and capture the exact SX1262 RF params ([02-hardware](02-hardware/README.md)).

Contributions welcome. Useful starting points: [own cloud + broker log](04-connect-your-own-cloud/README.md),
[MQTT topics](03-reverse-engineering/firmware-layout.md#mqtt-topics),
[LoRa energy payload](03-reverse-engineering/lora-protocol.md#energy-payload-cmd-19--22--23--24--25).
