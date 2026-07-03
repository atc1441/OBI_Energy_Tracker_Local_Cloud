# Eigene‑Cloud‑Tools (🇩🇪)

Self‑Hosting‑Toolkit. Erzeugt die **eigene** PKI zur Laufzeit — hier sind keine Secrets abgelegt. Die Tools
liegen unter [`../04-connect-your-own-cloud/tools/`](../04-connect-your-own-cloud/tools/).

| Tool | Rolle |
|---|---|
| `fetch_tea_key.py` | TEA‑Key aus der Cloud holen: **Email + Passwort + BLE‑Name → Key** (nur Stdlib) |
| `gen_certs.py` | Einmal‑PKI: CA, Server‑Cert, Claim‑Cert, permanentes („consistent") Cert + `ble_config.json` |
| `mqtts_server.py` | Minimaler MQTTS‑Broker (TLS 8883): spielt AWS‑IoT‑Fleet‑Provisioning nach, loggt alles und kann Downlinks pushen — `--set-interval N` (Reader‑Upload‑Rate) und `--ota-firmware fw.bin` (Image flashen) |
| `ble_provision.py` | Eigenes BLE‑Tool: findet `OBI-XXXXXX`, entbindet, **koppelt einen Reader** (Menü der gefundenen Reader; standardmäßig an), setzt WLAN + pusht Cert‑Config — alles in einer BLE‑Session |
| `obi_ota_download.py` | Zieht ein **Stock‑Firmware‑Image** aus der Hersteller‑Cloud (spielt den OTA‑Client des Geräts) — liefert etwas zum Flashen für `--ota-firmware` |
| `mitm_proxy.py` | Sitzt zwischen Gerät und echter Cloud, damit das Gateway in der App „echt" bleibt |

Installieren: `pip install cryptography bleak paho-mqtt`.

## Fake‑Cloud‑Flow (Gerät voll lokal)
```bash
python fetch_tea_key.py                                    # Key holen (oder UART cmd 49)
python gen_certs.py --host 192.168.1.50
python mqtts_server.py --host 0.0.0.0 --port 8883          # eigenes Terminal
python ble_provision.py --config pki/ble_config.json --key <TEA-KEY> --unbind \
    --pair-sensor --ssid <wlan> --password <wlan-pw>
```
`ble_provision.py`‑Flags: `--unbind` (bestehendes Cert löschen); das Reader‑Koppeln läuft **standardmäßig**
und zeigt ein **Menü der gefundenen Reader** zur Auswahl (`SensorScan`→`SensorBind` — **hier machen; BLE ist
im Betrieb aus und es gibt keinen MQTT‑Weg**). `--sensor-uuid <uuid>` oder `--first` binden nicht‑interaktiv,
`--no-pair-sensor` überspringt; außerdem `--scan-timeout <s>`, `--status-only`, `--frag <n>` (Fragmentgröße
verkleinern, falls der Cert‑Push timeoutet).

## Downlink‑Kommandos (mit einem laufenden Gerät über MQTT sprechen)
Sobald das Gerät an deinem Broker hängt, kann `mqtts_server.py` Kommandos zurückschicken (der Broker
injiziert sie selbst, da er nicht zwischen Clients routet):
```bash
python mqtts_server.py --host 0.0.0.0 --port 8883 --set-interval 60     # Reader-Upload alle 60 s (Standard 300)
python mqtts_server.py --host 0.0.0.0 --port 8883 --ota-firmware fw.bin # Image flashen (unsigniertes OTA)
```
Protokolle + Sicherheitshinweise: [03-cloud-api.md#downlink-kommandos-cloud--gerät](03-cloud-api.md#downlink-kommandos-cloud--gerät)
und [Firmware flashen](04-eigene-cloud.md#eigene-firmware-flashen).

Der Server beantwortet automatisch `$aws/certificates/create` (permanentes Cert) und den
Provisioning‑Publish (`thingName`); `--push-shadow` schickt zusätzlich ein `shadow/update/delta`. Loggt
jedes CONNECT (Client‑ID + Cert‑CN + MQTT‑Version), SUBSCRIBE und PUBLISH.

## MITM‑Flow (Gerät auch in der App sichtbar)
```bash
# Gerät zuvor einmal auf deinem Konto einbinden, dann in den Setup-Modus zurücksetzen
python gen_certs.py --host 192.168.1.50
python mitm_proxy.py --host 192.168.1.50 --port 8883 --uuid <geräte-uuid> \
    --cloud-endpoint <dein-geräte-cloud-endpoint> --cloud-dir <deine-cloud-certs>
python ble_provision.py --config pki/ble_config.json --key <TEA-KEY> --unbind --ssid <wlan> --password <pw>
```
`--no-cloud` fährt nur die Geräteseite. Der Proxy‑Log zeigt beide Richtungen.

## Hinweise
- `gen_certs.py` schreibt Klartext‑Schlüsselmaterial nach `pki/`; nur lokal halten (`.gitignore` blockt es).
- Nach **jedem** `gen_certs.py`‑Lauf **erneut pushen** (neue CA), bevor du den Server neu startest.
- Web‑Bluetooth‑Alternative zu `ble_provision.py`: [../06-tools/obi_gateway_ble.html](../06-tools/obi_gateway_ble.html).
