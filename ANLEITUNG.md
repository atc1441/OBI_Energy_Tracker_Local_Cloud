# 🇩🇪 Anleitung: Eigene Cloud in einem Durchgang ("1‑to‑Done")

Diese Seite führt dich **von Anfang bis Ende** durch: eigene MQTTS‑Cloud, Gerät umziehen, fertig. Einfach
Schritt für Schritt abarbeiten. Alle Keys/Zertifikate hier sind Platzhalter — du erzeugst deine eigenen.

> Nur für Geräte, die **dir gehören**. Siehe [DISCLAIMER.md](DISCLAIMER.md).

---

## Was du brauchst
- Die **Bridge** (ESP32‑C3 Gateway), den Reader zum koppeln und dein WLAN.
- Einen **Rechner im selben LAN** (dein Broker‑Host), Port **8883** offen.
- **Python 3** und einmalig:
  ```bash
  pip install cryptography bleak paho-mqtt
  ```
  Diesen MQTT-broker benutzt Du auf Deinem Rechner während der Einrichtung, um vom der Bridge zu lesen / darauf zu schreiben.
- Bluetooth am Rechner (für den BLE‑Push) **oder** ein Handy/Browser mit Web‑Bluetooth.
- Den **BLE‑Namen** des Geräts: `OBI-XXXXXX` (mit einem beliebigen BLE‑Scanner auslesen — er steht *nicht* auf dem Gerät).
- Ein heyObi-Konto **oder** ein UART interface an Deinem Rechner

---

## Schritt 1 — TEA‑Key des Geräts holen
Der Key verschlüsselt den BLE‑Steuerkanal. **Ein** Weg reicht:

**Weg A (am einfachsten, Gerät ist auf deinem OBI‑Konto):**
```bash
cd 04-connect-your-own-cloud/tools
python fetch_tea_key.py
#   OBI account email:            deine@mail.de
#   OBI account password:         ********
#   Device BLE name (OBI-XXXXXX): OBI-XXXXXX
#   -> TEA key for OBI-XXXXXX: 00112233445566778899AABBCCDDEEFF
```

**Weg B (ohne Konto, dafür physischer UART‑Zugang):**
Öffne [`06-tools/obi_uart_config.html`](06-tools/obi_uart_config.html) in Chrome/Edge, verbinde die
UART0‑Konsole (115200 8N1) und klicke **„Read IDs & TEA key"** (cmd 49). Der Key steht in der Antwort.

➡️ **Notiere dir den 32‑Hex‑TEA‑Key.**

---

## Schritt 2 — Eigene PKI erzeugen
Erzeugt CA, Server‑Cert (auf deine LAN‑IP), Claim‑Cert, ein festes „permanentes" Cert und `ble_config.json`:
```bash
cd 04-connect-your-own-cloud/tools
python gen_certs.py --host 192.168.1.50      # <-- die LAN-IP deines Broker-Rechners
```

---

## Schritt 3 — Eigenen MQTTS‑Broker starten
Läuft dauerhaft, spielt AWS‑IoT‑Fleet‑Provisioning nach und loggt alles:
```bash
python mqtts_server.py --host 0.0.0.0 --port 8883
```
(Eigenes Terminal offen lassen.)

---

## Schritt 4 — Gerät per BLE umziehen (WLAN + Zertifikate + Unbind + Reader koppeln)
In **einem** Aufruf: WLAN setzen, deine CA/Claim‑Cert/Broker‑URL pushen, vom alten Besitzer **entbinden**
und einen **Reader koppeln**:
```bash
python ble_provision.py --config pki/ble_config.json --key <DEIN-TEA-KEY> --unbind \
    --ssid <dein-wlan> --password <dein-wlan-passwort>
```
Der Reader muss eingeschaltet und in der Nähe der Bridge sein; das Reader‑Koppeln läuft **standardmäßig mit** (es muss — BLE geht aus, sobald das Gerät in Betrieb ist,
und es gibt **keinen MQTT‑Weg, einen Reader hinzuzufügen**, [07](de/07-reader-koppeln.md)). Das Tool scannt
und zeigt dann eine **Auswahl der gefundenen Reader**, aus der du den richtigen wählst:
```
[+] reader found (1): 0011...aa   (rssi=-71, battery=100)
[+] reader found (2): 0011...bb   (rssi=-93, battery=90)

  Pick a reader to bind [1-2], 'r'=rescan, 'q'=skip: 1
```
> ⚠️ Ein frisch gebundener Reader meldet erst nach ein paar Minuten (LoRa‑Join). Nicht‑interaktiv:
> `--sensor-uuid <uuid>` (bestimmten binden) oder `--first` (ersten gefundenen); `--no-pair-sensor` überspringt.

**Alternativ im Browser:** [`06-tools/obi_gateway_ble.html`](06-tools/obi_gateway_ble.html) öffnen, TEA‑Key
eintragen, und die `SetTMPCertificate`‑Felder aus `pki/ble_config.json` einfügen.

---

## Schritt 5 — Fertig ✅ — die eintreffenden Zählerdaten beobachten
Nach Schritt 4 verbindet sich die Bridge in dein WLAN → zu `mqtts://<deine-ip>:8883` → macht
`CreateKeysAndCertificate` + `RegisterThing` → bekommt dein festes Cert → verbindet neu → sendet Telemetrie.
**Lass das `mqtts_server.py`‑Fenster offen und schau ins Log.** Der Reihe nach:

```text
CONNECT   client_id='...'  cert_cn=...
PUBLISH   $aws/certificates/create/json        # Fleet-Provisioning
  -> reply .../accepted
PUBLISH   $aws/rules/EnergyTrackingBridge/<BRIDGE>/state
            {"uuid":"<BRIDGE>", ... "paired_sensor":[{"sensor":{... "sensor_upload_interval":300}}]}
...ein paar Minuten nachdem sich der Reader per LoRa verbunden hat...
PUBLISH   $aws/rules/EnergyTrackingSensor/bridge/<BRIDGE>/sensor/<SENSOR>/state
            {"uuid":"<SENSOR>","bridge_uuid":"<BRIDGE>", ... "timestamp":1700000000,
             "rssi":-83,"battery":100,"energy":12345678,"negative_energy":null,"power":null}
```

Diese letzte `EnergyTrackingSensor/.../state`‑Zeile mit einem **`energy`**‑Wert ≠ null ist dein Zählerstand,
der auf **deinem** Broker landet. Sie wiederholt sich alle `sensor_upload_interval` Sekunden (Standard 300).
Feld‑Bedeutung: [03‑cloud‑api.md](de/03-cloud-api.md#telemetrie-payloads-dekodiert--an-echtem-gerät-bestätigt).

**Schneller als alle 300 s?** Broker mit `--set-interval` starten; er schiebt die Änderung ans Gerät, sobald
es sein Command‑Topic abonniert (siehe [Downlink‑Kommandos](de/03-cloud-api.md#downlink-kommandos-cloud--gerät)):
```bash
python mqtts_server.py --host 0.0.0.0 --port 8883 --set-interval 60   # Zählerdaten alle 60 s
```

Ab jetzt läuft **alles über deinen Server** (Status, Konfig, OTA).

---

## Optional Schritt 6 — Eigene Firmware flashen
Direktes Flashen über UART/JTAG geht **nicht** (Bootloader per eFuse gesperrt). Aber das MQTT‑OTA ist
**unsigniert**, also flasht dein Gerät jedes Image aus *deiner* Cloud:
```bash
python mqtts_server.py --host 0.0.0.0 --port 8883 --ota-firmware fw.bin
```
```fw.bin``` ist Platzhalter für Dein gewähltes Firmware image. Das aktuellste hat ein wartungsfreundliches **Webinterface** mit dem Du z. B. den OBI Energy Tracker einfach mit Deinem **Home Assistant** verbinden kannst. Herunterladen bei [Releases](/releases).

Der Broker schickt den Offer, das Gerät zieht das Image in 512‑Byte‑Chunks und rebootet bei 100 %.
Stock‑Image zum Flashen holst du mit `tools/obi_ota_download.py`.
⚠️ Das flasht die Bridge wirklich neu — es ist möglich, das aktuelle Image sichern. Details:
[04 · Eigene Firmware flashen](de/04-eigene-cloud.md#eigene-firmware-flashen) ·
[Protokoll](de/03-cloud-api.md#ota).

---

## Troubleshooting
- **`bad certificate` / mbedTLS `-0x2700`:** Nach **jedem** `gen_certs.py` entsteht eine **neue CA** → 
  Schritt 4 (BLE‑Push) **erneut** ausführen, **dann** den Server neu starten. Server‑Cert muss die IP
  enthalten, zu der das Gerät verbindet (macht `gen_certs.py --host` automatisch).
- **Gerät verbindet nicht:** WLAN korrekt gesetzt? Firewall auf **8883** offen? Broker‑Host im selben Netz?
- **Kein TEA‑Key aus der Cloud:** Login/Passwort korrekt? BLE‑Name exakt `OBI-XXXXXX`? Das Gerät muss
  **nicht** auf deinem Konto sein — ein gültiges Login + der BLE‑Name reichen. Sonst Weg B (UART).
- Chain prüfen: `openssl verify -CAfile pki/ca.pem pki/server.crt` → muss `OK` sagen.

---

## Alternative: MITM (Gerät bleibt in der App „echt")
Statt die Cloud nur zu faken, sitzt `mitm_proxy.py` zwischen Gerät und echter Cloud — das Gerät bleibt in
der App online, während du in der Mitte alles siehst. Siehe
[04 · tools](04-connect-your-own-cloud/tools/README.md).

Vollständige Themen‑Doku auf Deutsch: **[de/README.md](de/README.md)**.
