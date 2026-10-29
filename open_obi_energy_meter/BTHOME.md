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
Pages stream from flash with `Content-Encoding: gzip` and `Vary: Accept-Encoding`; the
browser decompresses them. Page contents are unchanged, and no device-side decompressor is needed.
Because there is no decompressor, a request that rules gzip out (`Accept-Encoding: identity`,
`deflate`, `gzip;q=0`) is answered with `406 Not Acceptable` and a line of plain text rather than a
body it could not read. A request with no `Accept-Encoding` header still receives gzip, which RFC 9110
permits; `curl --compressed` is the convenient way to read a page from a shell.
CI checks the normal C3 image against both OTA slots and the history reserve.
The radio diagnostics ring retains 32 packets by default, saving 30 KiB of
static RAM compared with 96 entries. The browser still keeps its own history.
`OBI_RADIO_LOG_ENTRIES` can override this at build time where RAM permits;
larger rings may prevent classic ESP32 builds from linking with Bluetooth.

## Heap: Bluetooth or everything else

Measured on a live obi_gateway_c3, settled about 30 s after boot (`/api/status`):

| State | Free heap | Largest block |
| --- | --- | --- |
| Built without BTHome (`custom_bthome =`) | 132,300 | 102,400 |
| Built with BTHome, switched off at boot | 117,500 | 94,200 |
| Switched on, advertising | 65,300 | 45,000 |
| Switched off again at runtime | 114,200 | 47,100 |

Three things follow from that.

**Switching off returns the radio's heap at once.** `enabled=0` deinitializes the host stack and
releases what the controller still allows, so the roughly 49 KB NimBLE had taken comes back
immediately instead of staying occupied until the next restart.

**Switching on again needs a reboot.** Handing that memory to the heap is one-way per boot
(`esp_bt_mem_release` may only run before the controller has been started). A gateway that booted with
BTHome off therefore stores the new configuration, answers `"reboot_required": true` with
`"error": "Reboot required to start Bluetooth"`, and begins advertising after a restart.
`POST /api/reboot` is enough.

**Linking the code at all still costs about 15 KB, even switched off.** On the ESP32-C3 that is not the
controller's releasable BSS: `esp_bt_mem_release` returns only ~2 KB before the controller starts, and
the rest only disappears if NimBLE is not linked. Installations that will never use BTHome should leave
it out of the build. One line in the git-ignored `platformio.local.ini` does it:

```ini
[env]
custom_bthome =
```

That also saves about 126 KB of flash and changes nothing else in the firmware.

**Reader-firmware updates want one large contiguous block.** `gw_ota_begin()` allocates a whole reader
image in a single `malloc()` (~45 KB for the `reader_modded_*` images), so it is bounded by the largest
block, not by free heap. While BTHome advertises that block is around 45 KB, and a runtime switch-off
leaves it fragmented near 47 KB -- push reader firmware from a freshly rebooted gateway with BTHome off.
The gateway's own OTA is unaffected: it streams in 4 KiB writes and was verified updating a live C3
while BLE was running.

## Configure

The feature starts disabled. Pair and assign the meter through the dashboard
first: BTHome can only publish a reader the gateway is bound to.

Then open **Settings → BTHome** — the card right below EcoFlow STREAM:

1. Tick *Enabled* and pick the reader whose power should go out.
2. Press *Generate* for a private 16-byte bindkey and copy it: the receiving
   BTHome integration needs the very same key. The gateway keeps it in NVS and
   never hands it back out, so a key that is written down nowhere else is lost.
3. *Save*. The first time, the card then asks for one reboot — Bluetooth can
   only claim its memory while the device starts up, so an enable that arrives
   later has to wait for the next boot. *Reboot now* gets it over with.

On a later save, leaving the key field blank keeps the stored key. Switching the
feature off takes effect immediately and hands the heap back; switching it on
again always needs that reboot. The card shows the advertising state, the frame
count, the age of the last reading and any error, and hides itself entirely on a
build without `OBI_BTHOME`.

Configure over your trusted local network: the dashboard uses HTTP. Supply your
key to the receiver when it discovers `OBI-PWR`. The gateway has its own stable
Bluetooth MAC; it does not impersonate the old ESPHome device. Update receiver
entity references or automations accordingly.

The card is only a client of the authenticated web API, which also stands on its
own — handy for provisioning several gateways from a script. Keys are stored in
NVS, never returned by this API and never logged.

```sh
python3 -c 'import secrets; print(secrets.token_hex(16))'    # a bindkey
curl -X POST http://gateway/api/bthome \
     -d enabled=1 -d reader=010203 -d key=<32 hex> -d stale_seconds=60
```

| POST field | Meaning |
| --- | --- |
| `enabled` | `0` or `1`; defaults to disabled |
| `reader` | Exactly six hex characters; one assigned meter |
| `key` | Exactly 32 hex characters; omit to retain the saved key |
| `stale_seconds` | 5–3600 seconds; defaults to 60 |

Status: `GET /api/bthome`, which is what the settings card polls. It reports
configuration, Bluetooth MAC, advertising state, `reboot_required`, packet count,
sample age and errors. It never returns the key.

To disable, untick *Enabled* or send `enabled=0`. Changing configuration
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

Die Erweiterung ist zunächst ausgeschaltet. Eingestellt wird sie unter
*Einstellungen → BTHome*, in der Kachel unter EcoFlow STREAM: Reader auswählen,
mit *Erzeugen* einen privaten 32-stelligen Hex-Schlüssel anlegen, kopieren,
speichern. Danach ist einmalig ein Neustart nötig, weil Bluetooth seinen
Arbeitsspeicher nur beim Start belegen kann; die Kachel bietet dafür einen
Knopf. Anschließend erscheint die Bridge als `OBI-PWR`. Der Schlüssel liegt im
NVS und wird nie wieder herausgegeben — ohne eigene Kopie ist er verloren. Die
neue Bluetooth-Adresse muss im Empfänger eingerichtet werden. Dieselben
Einstellungen gibt es unverändert als `POST /api/bthome` und `GET /api/bthome`
mit der vorhandenen Dashboard-Anmeldung.

Build und Hosttests ersetzen keinen Funk- oder Dauertest. Vor einer Freigabe
sind die obigen Prüfungen am gekauften Gerät erforderlich. Bis dahin ist dies
eine experimentelle Erweiterung.
