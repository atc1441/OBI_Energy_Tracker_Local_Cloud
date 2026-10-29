# BTHome BLE power output

All normal gateway builds include optional encrypted BTHome v2 output, disabled
by default. Once enabled, the gateway advertises the selected LoRa reader's
instantaneous power using its ESP32 Bluetooth radio. The original reader stays
unchanged. This does not add Zigbee support.

**Status: compiled, host-tested and radio-tested on a generic ESP32-C3;
physical OBI hardware validation pending.**
The USB bench runs the production BLE modules with simulated readings. Actual
received packets passed authenticated decryption using PyCryptodome and Home
Assistant's `bthome-ble` parser for +1234, 0 and -850 W. BLE also continued with
the test Wi-Fi AP enabled/disabled, stopped after sample expiry and resumed
after restart with a higher persisted counter. See the
[bench instructions](examples/bthome_c3/README.md).

This does not establish RF range, full gateway heap usage, coexistence under
network traffic, reader compatibility or operation on an unknown OBI revision.

## Build

From this directory:

```sh
pio run -e obi_gateway_c3
```

The OTA application image is `.pio/build/obi_gateway_c3/firmware.bin`.
Use the original gateway's documented [OTA installation](README.md#stock-c3)
procedure. Check the actual device's inactive app partition before installing:
the build's `min_spiffs.csv` uses 1,966,080-byte slots, but OTA does not replace
the device's partition table. Leave the existing 256 KiB history tail reserve
intact when updating an open gateway. Never send a merged factory image through
the application updater.

Use your board's normal build target; no separate BLE firmware is needed.
NimBLE is pinned, with central, observer and peripheral roles disabled; only
advertising is used. Bluetooth initializes lazily after enabling the feature
and receiving a valid reading. Disabled installations never initialize it.

To preserve flash headroom, `embed_web.py` compresses the pages in `web/` during
each build. The generated header lives inside the environment's build directory.
Pages stream from flash with `Content-Encoding: gzip`; the browser decompresses
them. Page contents are unchanged, and no device-side decompressor is needed.
CI checks the normal C3 image against both OTA slots and the history reserve.
The radio diagnostics ring retains 32 packets by default, saving 30 KiB of
static RAM compared with 96 entries. The browser still keeps its own history.
`OBI_RADIO_LOG_ENTRIES` can override this at build time where RAM permits;
larger rings may prevent classic ESP32 builds from linking with Bluetooth.

## Configure

The feature starts disabled. Configuration uses the existing authenticated web
API; there is no additional settings panel. Pair and assign the meter through
the dashboard first. Its six-digit reader ID appears in `/api/readers`.

Generate a private 16-byte bindkey (32 hexadecimal characters) and keep it for
the receiving BTHome integration. For example, on your computer:

```sh
python3 -c 'import secrets; print(secrets.token_hex(16))'
```

Open the gateway dashboard, sign in if authentication is enabled, then execute
this in that page's browser console. The request uses the existing session.

```js
await fetch('/api/bthome', {
  method: 'POST',
  headers: {'Content-Type': 'application/x-www-form-urlencoded'},
  body: new URLSearchParams({
    enabled: '1',
    reader: '010203', // Replace with your assigned meter ID.
    key: prompt('BTHome bindkey (32 hex characters)'),
    stale_seconds: '60'
  })
}).then(r => r.json());
```

Configure over your trusted local network: the existing dashboard uses HTTP.
Keys are stored in the device's NVS, never returned by this API or logged.
Supply your key to the receiver when it discovers `OBI-PWR`. The gateway has
its own stable Bluetooth MAC; it does not impersonate the old ESPHome device.
Update receiver entity references or automations accordingly.

| POST field | Meaning |
| --- | --- |
| `enabled` | `0` or `1`; defaults to disabled |
| `reader` | Exactly six hex characters; one assigned meter |
| `key` | Exactly 32 hex characters; omit to retain the saved key |
| `stale_seconds` | 5–3600 seconds; defaults to 60 |

Status: `GET /api/bthome`. It reports configuration, Bluetooth MAC, advertising
state, packet count, sample age and errors. It never returns the key.

To disable, send `enabled=0` to the same POST endpoint. Changing configuration
stops advertisements and waits for the next valid reading. Authentication
follows the dashboard: without a configured login, its APIs are open locally.

## Data and timing

- Only instantaneous power is advertised: object `0x5C`, signed 32-bit,
  0.01 W units. Positive means import, negative means export. Integer watt
  readings from the reader are scaled by 100; this adds no measurement precision.
- No estimated power or artificial zero replaces an invalid reading. Invalid
  power, inactive IR, unassigned readers and expired samples stop advertising.
- New readings refresh the advertisement, at most every 100 ms. Between readings,
  a fresh encrypted packet is emitted every second while the sample is valid.
  BLE repeats it at 100–150 ms intervals. This does not increase the reader's
  measurement rate; set the LoRa upload interval separately in the dashboard.
- Each advertising burst has a controller-enforced deadline of at most two
  seconds, capped by the sample's remaining lifetime. A blocked web task stops
  refreshing BLE; it cannot keep an old advertisement running indefinitely.
  Long dashboard/network operations may therefore introduce advertising gaps.
- Wi-Fi/MQTT/cloud delivery is not required for BLE output after setup. The
  existing dashboard remains available for configuration and firmware updates.
- The receiver may retain its last value after advertisements stop. Its timeout
  and any control-loop fallback must be tested separately.

## Encryption and counters

The frame follows the [BTHome encryption specification](https://bthome.io/encryption/):
`UUID | 0x41 | ciphertext | counter | MIC`. The AES-CCM nonce uses the public
Bluetooth MAC in display order, little-endian service UUID and counter. There is
no V1 associated-data byte. This also covers the counter/MIC ordering regression.

Before using a counter, the gateway durably reserves a block of 4096 values in
NVS. Reboots skip the unused remainder. Counter storage survives configuration
changes and the normal factory reset, even if the same key is entered again.
Storage errors and 32-bit exhaustion stop output. No unencrypted fallback exists.

Erasing all NVS or restoring an old full-flash backup can roll counters back.
Use a new bindkey after either operation. Do not clone the same MAC and key to
another running device.

## Automated checks

On a host with Python 3.11+, a C++ compiler and OpenSSL development headers:

```sh
python -m pip install -r tests/requirements.txt
python -m pytest tests -q
pio run -e obi_gateway_c3
python tests/verify_bthome_image.py .pio/build/obi_gateway_c3
```

The native tests compile the actual packet builder and runtime module. Hardware
interfaces are replaced by deterministic fakes; the crypto shim uses OpenSSL.
PyCryptodome and Home Assistant's `bthome-ble` parser independently decrypt the
result. The real ESP32 build compiles the mbedTLS and NimBLE paths.

Coverage includes positive/negative/zero watts, scaling limits, absent readings,
authentication failures, frame layout, MAC byte order, counter persistence and
exhaustion, configuration validation, selected-reader filtering, bounded
advertising lifetime, stale readings, reset behaviour and storage failures.
Undefined-behaviour checks run with traps enabled in the native binaries.

## Remaining OBI hardware acceptance

1. Record bridge/reader hardware and firmware versions; verify the OTA path,
   actual partition sizes and recovery/backup procedure before flashing.
2. Run the baseline firmware, pair the reader and compare signed watts against
   the existing ESPHome reader under import and export.
3. Enable BTHome, configure one reader and a fresh key, and confirm
   discovery/decryption at the intended receiver location.
4. Compare the BLE value and timing with dashboard power. Test zero, import,
   export and the desired LoRa interval, with USB reader power if needed.
5. Restart and power-cycle the bridge. Verify the MAC remains stable, counters
   increase and the receiver resumes without re-pairing.
6. Remove the reader or obstruct IR. Verify BLE output stops within the timeout
   and the receiving automation handles stale data appropriately.
7. Test with Wi-Fi/MQTT unavailable and under web/MQTT activity. Observe free
   heap, reconnects, missed LoRa readings and advertising gaps over 24 hours.
8. Test disable/re-enable and an OTA update. Record these results before
   considering the complete OBI gateway hardware-validated.

## Deutsch

Alle normalen Gateway-Builds enthalten verschlüsseltes BTHome, standardmäßig
ausgeschaltet. Für die originale Bridge bleibt der Build `obi_gateway_c3`.
Der Lesekopf sendet weiterhin LoRa. Übertragen wird die aktuelle
vorzeichenbehaftete Leistung, entsprechend `power_sint32` in ESPHome.

Die Erweiterung ist zunächst ausgeschaltet. Konfiguration erfolgt über
`POST /api/bthome`, Status über `GET /api/bthome`, jeweils mit der vorhandenen
Dashboard-Anmeldung. Nach Auswahl des Readers und Eingabe eines privaten
32-stelligen Hex-Schlüssels erscheint die Bridge als `OBI-PWR`. Die neue
Bluetooth-Adresse muss im Empfänger eingerichtet werden.

Build und Hosttests ersetzen keinen Funk- oder Dauertest. Vor einer Freigabe
sind die obigen Prüfungen am gekauften Gerät erforderlich. Bis dahin ist dies
eine experimentelle Erweiterung.
