# Own-cloud tools

Self-hosting toolkit. Generates your **own** PKI at runtime — no secrets are shipped here.

| Tool | Role |
|---|---|
| `fetch_tea_key.py` | Get the device TEA key from the cloud: **email + password + BLE name → key** (stdlib only) |
| `gen_certs.py` | One-shot PKI: CA, server cert, claim cert, permanent ("consistent") cert + `ble_config.json` |
| `mqtts_server.py` | Minimal MQTTS broker (TLS 8883): emulates AWS-IoT fleet provisioning, logs everything, and can push downlinks — `--set-interval N` (reader upload rate) and `--ota-firmware fw.bin` (flash an image) |
| `ble_provision.py` | Your own BLE client: finds `OBI-XXXXXX`, unbinds, **pairs a reader** (menu of found readers; on by default), sets WiFi + pushes your cert config — all in one BLE session |
| `obi_ota_download.py` | Pull a **stock firmware image** from the vendor cloud (replays the device's OTA client) — get something to flash with `--ota-firmware` |
| `mitm_proxy.py` | Sits between device and real cloud so the bridge stays "real" in the app while you observe/modify |

Install: `pip install cryptography bleak paho-mqtt`.

## Fake-cloud flow (device fully local)
```bash
python gen_certs.py --host 192.168.1.50
python mqtts_server.py --host 0.0.0.0 --port 8883        # separate terminal
python ble_provision.py --config pki/ble_config.json --key <TEA-KEY> --unbind \
    --ssid <wifi> --password <wifi-pw>
```
`ble_provision.py` flags: `--unbind` (clear existing cert); reader pairing runs **by default** and shows a
**menu of the readers it found** so you pick one (`SensorScan`→`SensorBind` — **do it here, BLE is off
during normal operation and there's no MQTT way to add a reader**). `--sensor-uuid <uuid>` or `--first`
bind non-interactively, `--no-pair-sensor` skips it; also `--scan-timeout <s>`, `--status-only`, `--frag <n>`
(reduce fragment size if the cert push times out).

## Downlink commands (talk to a running device over MQTT)
Once the device is on your broker, `mqtts_server.py` can push commands back to it (it injects them itself,
since the minimal broker doesn't route between clients):
```bash
python mqtts_server.py --host 0.0.0.0 --port 8883 --set-interval 60     # reader upload every 60 s (default 300)
python mqtts_server.py --host 0.0.0.0 --port 8883 --ota-firmware fw.bin # flash an image (unsigned OTA)
```
Protocols + safety notes: [../../03-reverse-engineering/cloud-api.md#downlink-commands-cloud--device](../../03-reverse-engineering/cloud-api.md#downlink-commands-cloud--device)
and [flash firmware](../README.md#flash-your-own-firmware).

The server auto-answers:
- `PUBLISH $aws/certificates/create/json[/csr]` → `.../accepted` with the permanent cert/key.
- `PUBLISH $aws/provisioning-templates/<tpl>/provision/json` → `.../accepted` with `thingName`.
- `--push-shadow` optionally sends a `shadow/update/delta` so the device also gets a downlink.

Logs every CONNECT (client-id + cert CN + MQTT version), SUBSCRIBE, and PUBLISH (topic + payload). MQTT
3.1.1 and 5.0, sequential connections (claim → provision → reconnect) all handled.

## MITM flow (device also visible in the vendor app)
The proxy runs the device side (local provisioning) **and** a cloud side (a paho client acting as the
device's thing) and bridges uplinks/downlinks, so the bridge appears online in the app while you sit in
the middle.
```bash
# device must have been bound once on your account first, then reset to setup mode
python gen_certs.py --host 192.168.1.50
python mitm_proxy.py --host 192.168.1.50 --port 8883 --uuid <device-uuid> --cloud-dir <your-cloud-certs>
python ble_provision.py --config pki/ble_config.json --key <TEA-KEY> --unbind --ssid <wifi> --password <pw>
```
`--no-cloud` runs only the local side. The proxy log shows both directions.

**`--cloud-dir` (the real-cloud cert).** The cloud side authenticates as the device's own AWS-IoT
thing, so you need that thing's client cert — the same fleet-provisioning credentials the vendor app
pushes to the device. There is **no bundled script that provisions against the vendor cloud** (removed
on purpose — it would be a client that mints certs on OBI's infrastructure). Fetch them yourself for a
bridge **your account already owns** with an authenticated app request
`POST /device-provisionings` (body `{"bridgeId":"<uuid>"}`) →
`{caPem, certificatePem, privateKey, clusterEndpointUri, provisioningTemplateName}`, and drop the PEMs
into the dir as `ca.pem` / `client.crt` / `client.key` (`clusterEndpointUri` host → `--cloud-endpoint`).
Same convention as `obi_ota_download.py --from-dir`; details in
[../../03-reverse-engineering/cloud-api.md](../../03-reverse-engineering/cloud-api.md#where-the-device-secrets-come-from).

## Test without a device (simulator)
A companion MQTT client can run the same provisioning flow to verify the broker without hardware — see
the comments in `mqtts_server.py`.

## Notes
- `gen_certs.py` writes cleartext key material into `pki/`; keep it local.
- After **any** `gen_certs.py` re-run, **re-push** with `ble_provision.py` (new CA) before restarting the
  server, or the device rejects the new server cert.
- Web-Bluetooth alternative to `ble_provision.py`: [../../06-tools/obi_gateway_ble.html](../../06-tools/obi_gateway_ble.html).
