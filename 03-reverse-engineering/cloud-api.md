# The vendor cloud (REST API)

Reconstructed from the app for interoperability — this is what the phone app and the bridge talk to.
Documented so you can (a) legitimately pull *your own* device's key/certs and (b) understand what to
replicate on your own broker. No credentials here.

## Auth
- **Login:** `POST https://www.obi.de/regi/auth/api/public/login`
  - Body: `{"email","password","country":"DE"}`
  - Headers: `x-app-type: b2c`, `x-obi-locale: de-DE`, a `User-Agent` like `heyOBI APP / Android Phone 30`
  - Response: `{"token":"<JWT>"}`
- **API calls:** `Authorization: Bearer <token>`, `User-Agent: app_client`,
  `Accept: application/vnd.obi.companion.energy-tracking.<resource>.v1+json`.
- The JWT carries `accountId` (used as the user id by the energy backend), `email`, `tenant`, etc.

## Environments (host resolution)
| Env | Backend host | Livemode host |
|---|---|---|
| **prod** | `energy-tracking-backend.prod-eks.dbs.obi.solutions` | `energy-tracking-livemode.prod-eks.dbs.obi.solutions` |
| prod gateway | `api.obi.com` | – |
| stage / dev | `…stage-eks…` / `…dev-eks…` | internal / VPC-only |

Only **prod** is publicly reachable; stage/dev are internal.

## Media types
`application/vnd.obi.companion.energy-tracking.<resource>.v1+json` — resources: `user`, `bridge`,
`sensor`, `firmware-update`, `firmware-update-request`, `bluetooth-challenge`, `device-provisioning`,
`energy-consumer`, `historical-record`, `analytics-forecast`, `analytics-standby`.

## Endpoints (selection)
| RPC | Method | Path |
|---|---|---|
| Get user | GET | `/users/{accountId}` |
| List / get / update bridge | GET/GET/PUT | `/bridges` · `/bridges/{id}` |
| Release bridge | POST | `/bridges/{id}/release` |
| Get / update sensor | GET/PUT | `/sensors/{id}` |
| Reset meter reading | POST | `/sensors/{id}/reset` |
| **Available firmware** | GET | `/firmware-updates/{bridgeId}` → `{id, version, changeLog}` |
| **Trigger OTA** | POST | `/firmware-updates/{bridgeId}/trigger` — body `{firmwareId}` |
| **Device provisioning** | POST | `/device-provisionings` — body `{bridgeId}` |
| **BT challenge (TEA key)** | POST | `/bluetooth-challenges` — body `{"btChallengeId":"OBI-XXXXXX"}` → `{key}` |
| Historical meter / interval | GET | `/historical-data/{bridge}/{device}/meter` · `/{interval}` |
| Analytics | GET | `/analytics/{a}/{b}/forecast` · `/{c}/standby` |
| Energy consumers | GET/POST/PUT/DELETE | `/energy-consumers/{id}` |

## Where the device secrets come from
- **`/bluetooth-challenges`** returns the **16-byte TEA key** for the device named by
  `btChallengeId = OBI-XXXXXX` (its BLE advertising name). In practice this needs only a valid OBI login
  **plus the BLE name** — the endpoint does **not** verify the device is registered to your account (weak
  authorization, see [security-notes.md](security-notes.md)). This is the "cloud path" in
  [../04-connect-your-own-cloud/README.md](../04-connect-your-own-cloud/README.md#step-0--get-the-devices-tea-key).
- **`/device-provisionings`** returns the AWS-IoT fleet-provisioning credentials the app pushes to the
  device: `{caPem, certificatePem, privateKey, clusterEndpointUri, provisioningTemplateName}`. For your
  own cloud you generate these yourself with `gen_certs.py` instead.

## What the bridge itself publishes (MQTT, over TLS 8883)
Telemetry and OTA ride MQTT topics (see [firmware-layout.md](firmware-layout.md#mqtt-topics)):
`$aws/rules/EnergyTrackingBridge[Heartbeat]/<UUID>/state`,
`$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state`, and the `…/ota/firmware-*` topics.
Fleet provisioning uses `$aws/certificates/create/json` and
`$aws/provisioning-templates/<template>/provision/json`.

## Telemetry payloads (decoded — confirmed on a live device)
All telemetry is **JSON** (not binary). Three shapes:

**Bridge state / heartbeat** → `$aws/rules/EnergyTrackingBridge/<UUID>/state` and
`…EnergyTrackingBridgeHeartbeat/<UUID>/state` (firmware `mqtt_build_bridge_state`):
```json
{ "uuid": "<bridge>", "hardware_version": "6.0.0", "firmware_version": "1.0.1", "ota": null,
  "paired_sensor": [ { "sensor": null | {
      "uuid": "<sensor>", "hardware_version": "6.0.0", "firmware_version": "32.0.0",
      "online": true, "battery": 100, "ota": null, "sensor_upload_interval": 300 } } ] }
```
`sensor` is `null` until a reader is bound **and** reporting.

> **Schema note — firmware 1.2.x (`schema_version: 2`).** The `1.2.x` branch renames/extends this: the
> bridge state gains `"schema_version": 2` and an `"ota"` status field, `paired_sensor[].sensor` becomes
> `paired_devices[]` with a `"device"` type (`"meter"`, and 1.2.x also adds smart-**outlet** support),
> `sensor_upload_interval` becomes `upload_interval`, and the device subscribes to wildcard topics
> (`sensor/+/…`, `outlet/+/control`, `outlet/+/upload-interval-change-request`). Confirmed live after
> flashing a bridge to 1.2.1: it also populated **`negative_energy`** (export) which was `null` on 1.0.x.
> The `1.0.x` shape above is what this repo primarily documents; re-verify field names on `1.2.x`.

#### Firmware 1.2.x telemetry, reversed (schema_version 2)
Decoded from the 1.2.1 builders `mqtt_build_bridge_state_v12`, `mqtt_build_outlet_state`, and the sensor
publisher. Device type in `paired_devices[].device` is `"meter"` (type `0x10`) or `"outlet"` (`0x11`).

**Bridge heartbeat** → `$aws/rules/EnergyTrackingBridge/<UUID>/state` (cmd 3) and `…Heartbeat/…/state` (cmd 4):
```json
{ "uuid": "<bridge>", "hardware_version": "6.0.0", "firmware_version": "1.2.1", "ota": null,
  "schema_version": 2,
  "paired_devices": [
    { "device": "meter",  "battery": 100, "uuid": "<sensor>", "hardware_version": "6.0.0",
      "firmware_version": "57.0.0", "online": true, "ota": null, "upload_interval": 300 },
    { "device": "outlet", "battery": null, "uuid": "<outlet>", "hardware_version": "…",
      "firmware_version": "…", "online": true, "ota": null, "upload_interval": 300,
      "relay": "on" | "off" | null } ] }
```
A `meter` carries `battery`; an `outlet` carries `relay` (and `battery: null`). `online` = the device's
last-seen counter ≥ 2.

**Smart-outlet state** → `$aws/rules/EnergyTrackingOutlet/bridge/<UUID>/outlet/<UUID>/state` (cmd 6) and
`dt/…/outlet/<UUID>/state/live` (cmd 8) — `mqtt_build_outlet_state`:
```json
{ "uuid": "<outlet>", "bridge_uuid": "<bridge>", "hardware_version": "…", "firmware_version": "…",
  "online": true, "timestamp": 1700000000, "rssi": -70,
  "relay": "on" | "off",
  "voltage": 230000,        // mV
  "current": 512,           // mA
  "power": 118,             // W (signed)
  "energy": 12345,          // cumulative import
  "negative_energy": 0 }    // cumulative export
```
The reader/meter energy payload is unchanged from 1.0.x except `sensor_upload_interval`→`upload_interval`
in the bridge state; values come from `processMeterData` (`pos power / neg power / power / interval /
time delta`, plus `softver, hardver, voltage, infrared, lowpower`).

**Sensor energy (the meter readings)** → `$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state`
(periodic, every `sensor_upload_interval` s) and `dt/…/sensor/<UUID>/state/live` (live; every 6th frame is
"full") — firmware `mqtt_build_sensor_energy`:
```json
{ "uuid": "<sensor>", "bridge_uuid": "<bridge>", "hardware_version": "6.0.0",
  "firmware_version": "32.0.0", "online": true,
  "timestamp": 1700000000,        // Unix epoch (seconds)
  "rssi": -83,                    // LoRa signal from the reader
  "battery": 100,
  "energy": 12345678,             // cumulative import  (OBIS 1.8.0), raw meter register
  "negative_energy": null,        // cumulative export  (OBIS 2.8.0), null = no data / no export
  "power": null }                 // instantaneous power (OBIS 16.7.0), null = not in this frame
```
`energy` / `negative_energy` / `power` are emitted as **`null`** when the value is `0x7FFFFFFF` (no data)
or the reader is inactive; `power` typically populates in the "full/live" frames. To find the **unit** of
`energy`, compare the raw value to your meter's 1.8.0 display (it's the reader's raw register — likely Wh
or 0.001 kWh depending on the meter). Example values above are placeholders/illustrative.

## Downlink commands (cloud → device)
The device also **subscribes** to command topics and acts on JSON published to them. The one that is
fully reversed and verified is the **reader upload-interval change** (how often the reader's energy is
pushed — the "300 s" period).

**Change the reader upload interval** — the cloud publishes to the topic the device subscribes to:
```
cmd/energy-tracking/bridge/<BRIDGE-UUID>/sensor/<SENSOR-UUID>/upload-interval-change-request
```
```json
{ "sensor_upload_interval": 60, "session_id": 1 }
```
- `sensor_upload_interval` — new period in **seconds**, read as a **u16** (1..65535). This is the same
  field you see reported in the bridge-state `paired_sensor[].sensor.sensor_upload_interval`.
- `session_id` — an integer you choose; the device stores it and echoes it back so you can correlate.

Firmware handler: `mqtt_set_upload_interval` (protocol 2, cmd 6) parses the JSON, calls the internal
`set_upload_interval(sensor_upload_interval)`, stores `session_id`, and **replies** on
`…/upload-interval-change-response`:
```json
{ "uuid": "<sensor>", "bridge_uuid": "<bridge>", "sensor_upload_interval": 60,
  "session_id": 1, "timestamp": 1700000000 }
```
The new interval is then reflected in the next bridge-state heartbeat.

**Sending it from the local broker.** Because our minimal broker doesn't route between clients, the
command is injected by the broker itself: `mqtts_server.py --set-interval <seconds>` waits until the
device subscribes to its `…/upload-interval-change-request` topic and publishes the payload straight
back to it (see [../04-connect-your-own-cloud/tools/mqtts_server.py](../04-connect-your-own-cloud/tools/mqtts_server.py)):
```bash
python mqtts_server.py --set-interval 60          # ask for 60 s instead of 300
```
Watch the log for `-> SET sensor_upload_interval = 60s` and then a `…/upload-interval-change-response`
PUBLISH from the device confirming it (verified live: the device replies with the exact response schema
above and the next bridge-state heartbeat carries `"sensor_upload_interval": 60`).

> **Firmware 1.2.x (multi-sensor).** The `1.2.x` bridge supports several sensors (and smart outlets), so
> the command changed: the device **subscribes with a wildcard** (`cmd/…/sensor/+/upload-interval-change-request`)
> and the request payload **must include the target sensor `uuid`** — publish to the *concrete*
> `…/sensor/<SENSOR-UUID>/…` topic with `{"uuid":"<SENSOR-UUID>","sensor_upload_interval":15,"session_id":15}`
> (handler `mqtt_set_sensor_interval_v12` / `dealMeterIntervalCommand`; without `uuid` it logs
> `get uuid failed`). The response uses the renamed field: `{uuid, device:"meter", bridge_uuid, session_id,
> upload_interval, timestamp}`. There is a parallel `…/outlet/…` command for smart outlets that uses
> `upload_interval`. `mqtts_server.py --set-interval` handles all of this: it learns the sensor uuid from
> telemetry, targets the concrete topic, and includes `uuid`. Verified live: a 1.2.1 bridge accepted `15 s`
> and echoed `upload_interval: 15`.

> **Timing (verified live):** the ack and the heartbeat update are immediate. `sensor_upload_interval`
> controls **how often the bridge publishes** `EnergyTrackingSensor/…/state` — it re-publishes its latest
> cached reading at that rate. The **reader's own LoRa report rate is separate** (~300 s on the unit
> tested), so the `energy` *value* only changes when the reader next reports; between reports the bridge
> just re-emits the same value more frequently. Right after a change the bridge flushes the windows it
> missed, so you may see a short burst of identical readings, then steady spacing at the new interval.

### OTA firmware update (push an image over MQTT) <a id="ota"></a>
Fully reversed and verified (byte-for-byte against a real cloud capture + an end-to-end self-test). The
bridge self-update is **unsigned** — only an integrity hash — so a device on **your own cloud** will flash
any image you serve. This is the custom-firmware path. Four topics under `…/bridge/<UUID>/ota/`:

| Direction | Topic | Payload |
|---|---|---|
| cloud → device | `cmd/…/ota/firmware-update-request` | **23 bytes** binary: `[maj][min][patch] + total_len(u32 BE) + md5(16)` |
| device → cloud | `cmd/…/ota/firmware-update-response` | echoes the same 23 bytes (ack) |
| device → cloud | `dt/…/ota/firmware-data-request` | JSON `{"uuid","firmware_version":"X.Y.Z","offset":N}` |
| cloud → device | `dt/…/ota/firmware-data-response` | **9-byte header** `[maj][min][patch] + offset(u32 BE) + len(u16 BE)` + up to **512** image bytes |

Flow (firmware handlers `mqtt_ota_file_info`, `mqtt_ota_data`, requester `sub_4200E6AE`):
1. Cloud publishes the 23-byte **offer**. The version is cosmetic (the device doesn't require it to be
   newer); `total_len` **must** equal the image size — the device uses it to know when it's done.
2. Device echoes it back (ack), `esp_ota_begin`s, and starts **pulling**: it publishes a data-request for
   `offset:0`, then after each written chunk requests the next `offset` (`+512`). If a chunk doesn't arrive
   within ~10 s it re-requests (up to 5 tries, then aborts).
3. Cloud answers each request with `firmware[offset:offset+512]` behind the 9-byte header. The header's
   version bytes must match the offer (device does a 3-byte `memcmp`) and the `offset` must equal the
   device's current write position.
4. When `offset >= total_len` the device runs `esp_ota_end` → `esp_ota_set_boot_partition` → **reboots**
   into the new image. An aborted transfer never switches the boot partition, so it's safe to stop early.

Real offer from a capture: `01000200173d74d94189299932921089a2e041ed8e145c` → v1.0.2, `0x00173D74` =
1,522,548 bytes, then the 16-byte md5. The image begins with `0xE9` (ESP32 app magic).

**Serving it from the local broker:** `mqtts_server.py --ota-firmware fw.bin` pushes the offer when the
device subscribes and answers every data-request — see
[../04-connect-your-own-cloud/README.md](../04-connect-your-own-cloud/README.md#flash-your-own-firmware).
To obtain a stock image to flash (or study), pull it from the vendor with
[`obi_ota_download.py`](../04-connect-your-own-cloud/tools/obi_ota_download.py) (plays the device side).

## Historical-data parameters
- `duration`: ISO-8601 interval, e.g. `2026-07-01T23:00:00Z/PT24H`
- `measures`: `energy`, `negative_energy` (comma-separated)

> All of this is reversed from the client for interoperability with a device you own. Use your own
> account and your own hardware.
