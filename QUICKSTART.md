# Quickstart: Your own cloud in one pass ("1‑to‑Done")

Start to finish: your own MQTTS cloud, device moved over, done. Work top to bottom. All keys/certs are
placeholders — you generate your own. 🇩🇪 **Deutsche Version: [ANLEITUNG.md](ANLEITUNG.md).**

> Devices you own only. See [DISCLAIMER.md](DISCLAIMER.md).

## What you need
- The **bridge** (ESP32‑C3 gateway) and your WiFi.
- A **machine on the same LAN** (your broker host), port **8883** open.
- **Python 3** + once: `pip install cryptography bleak paho-mqtt`.
- Bluetooth on that machine (BLE push) **or** a Web‑Bluetooth browser.
- The device **BLE name** `OBI-XXXXXX` (read it with any BLE scanner — it is *not* printed on the device).

## Step 1 — get the TEA key (one way is enough)
```bash
cd 04-connect-your-own-cloud/tools
python fetch_tea_key.py     # email + password + BLE name -> 32-hex TEA key
```
No account? Read it over UART instead: open [`06-tools/obi_uart_config.html`](06-tools/obi_uart_config.html)
and click **Read IDs & TEA key** (cmd 49).

## Step 2 — generate your PKI
```bash
python gen_certs.py --host 192.168.1.50      # your broker's LAN IP
```

## Step 3 — start your MQTTS broker (leave running)
```bash
python mqtts_server.py --host 0.0.0.0 --port 8883
```

## Step 4 — move the device over (WiFi + certs + unbind + pair a reader)
```bash
python ble_provision.py --config pki/ble_config.json --key <YOUR-TEA-KEY> --unbind \
    --ssid <your-wifi> --password <your-wifi-pw>
```
Reader pairing runs **by default** (it must — BLE turns off once the device is operational, and there is
**no MQTT way to add a reader**). The tool scans, then shows a **menu of the readers it found** so you pick
which one to bind:
```
[+] reader found (1): 0011...aa   (rssi=-71, battery=100)
[+] reader found (2): 0011...bb   (rssi=-93, battery=90)

  Pick a reader to bind [1-2], 'r'=rescan, 'q'=skip: 1
```
Non-interactive instead: `--sensor-uuid <uuid>` (a specific one) or `--first` (first seen); `--no-pair-sensor`
to skip. A freshly bound reader takes a few minutes to report. Browser alternative:
[`06-tools/obi_gateway_ble.html`](06-tools/obi_gateway_ble.html).

## Step 5 — done ✅ — watch the meter data arrive
The bridge joins your WiFi → `mqtts://<your-ip>:8883` → provisions → reconnects with your permanent cert →
publishes telemetry. **Leave the `mqtts_server.py` window open and watch the log.** You'll see, in order:

```text
CONNECT   client_id='...'  cert_cn=...
PUBLISH   $aws/certificates/create/json        # fleet provisioning
  -> reply .../accepted
PUBLISH   $aws/rules/EnergyTrackingBridge/<BRIDGE>/state
            {"uuid":"<BRIDGE>", ... "paired_sensor":[{"sensor":{... "sensor_upload_interval":300}}]}
...a few minutes after the reader joins over LoRa...
PUBLISH   $aws/rules/EnergyTrackingSensor/bridge/<BRIDGE>/sensor/<SENSOR>/state
            {"uuid":"<SENSOR>","bridge_uuid":"<BRIDGE>", ... "timestamp":1700000000,
             "rssi":-83,"battery":100,"energy":12345678,"negative_energy":null,"power":null}
```

That last `EnergyTrackingSensor/.../state` line, with a non-null **`energy`**, is your meter reading landing
on **your** broker. It repeats every `sensor_upload_interval` seconds (default 300). Payload fields are
decoded in [cloud-api.md](03-reverse-engineering/cloud-api.md#telemetry-payloads-decoded--confirmed-on-a-live-device).

**Want it faster than every 300 s?** Start the broker with `--set-interval`; it pushes the change back to
the device as soon as it subscribes (see [downlink commands](03-reverse-engineering/cloud-api.md#downlink-commands-cloud--device)):
```bash
python mqtts_server.py --host 0.0.0.0 --port 8883 --set-interval 60   # meter data every 60 s
```

## Optional — flash your own firmware
The MQTT self-update is unsigned, so a device on your cloud will flash any image you serve:
```bash
python mqtts_server.py --host 0.0.0.0 --port 8883 --ota-firmware fw.bin
```
It sends the offer, the device pulls the image in 512-byte chunks and reboots at 100 %. Get a stock image
with `tools/obi_ota_download.py`. ⚠️ This really reflashes the bridge — back up the current image first.
Details: [04 · Flash your own firmware](04-connect-your-own-cloud/README.md#flash-your-own-firmware) ·
[protocol](03-reverse-engineering/cloud-api.md#ota).

## Troubleshooting
- **`bad certificate` / mbedTLS `-0x2700`:** every `gen_certs.py` run makes a **new CA** → re‑run Step 4,
  then restart the server. Server cert must cover the IP the device connects to (`--host` handles it).
- **No connect:** WiFi set? firewall on 8883? host on the same network?
- **No key from cloud:** check your login and the exact BLE name (`OBI-XXXXXX`) — the device need **not**
  be on your account. Otherwise use the UART way.
- Verify chain: `openssl verify -CAfile pki/ca.pem pki/server.crt` → `OK`.

Full docs: [04 · Connect your own cloud](04-connect-your-own-cloud/README.md).
