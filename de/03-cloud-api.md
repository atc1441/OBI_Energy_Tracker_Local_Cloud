# Die Hersteller‑Cloud (REST‑API) (🇩🇪)

Aus der App rekonstruiert (Interoperabilität) — das sprechen App und Gateway. Dokumentiert, damit du (a)
legitim den Key/die Certs *deines eigenen* Geräts holen und (b) verstehen kannst, was du auf dem eigenen
Broker nachbauen musst. Keine Credentials hier.

## Auth
- **Login:** `POST https://www.obi.de/regi/auth/api/public/login`
  - Body: `{"email","password","country":"DE"}`
  - Header: `x-app-type: b2c`, `x-obi-locale: de-DE`, `User-Agent` wie `heyOBI APP / Android Phone 30`
  - Antwort: `{"token":"<JWT>"}`
- **API‑Calls:** `Authorization: Bearer <token>`, `User-Agent: app_client`,
  `Accept: application/vnd.obi.companion.energy-tracking.<resource>.v1+json`.
- Das JWT trägt `accountId` (User‑ID im Energy‑Backend), `email`, `tenant`, …

## Environments (Host‑Auflösung)
| Env | Backend‑Host | Livemode‑Host |
|---|---|---|
| **prod** | `energy-tracking-backend.prod-eks.dbs.obi.solutions` | `energy-tracking-livemode.prod-eks.dbs.obi.solutions` |
| prod‑Gateway | `api.obi.com` | – |
| stage / dev | `…stage-eks…` / `…dev-eks…` | intern / VPC‑only |

Nur **prod** ist öffentlich erreichbar; stage/dev sind intern.

## Media‑Types
`application/vnd.obi.companion.energy-tracking.<resource>.v1+json` — Ressourcen: `user`, `bridge`,
`sensor`, `firmware-update`, `bluetooth-challenge`, `device-provisioning`, `energy-consumer`,
`historical-record`, `analytics-forecast`, `analytics-standby`.

## Endpunkte (Auswahl)
| RPC | Methode | Pfad |
|---|---|---|
| User holen | GET | `/users/{accountId}` |
| Bridges list/get/update | GET/GET/PUT | `/bridges` · `/bridges/{id}` |
| Bridge freigeben | POST | `/bridges/{id}/release` |
| Sensor get/update | GET/PUT | `/sensors/{id}` |
| Zählerstand reset | POST | `/sensors/{id}/reset` |
| **Verfügbare Firmware** | GET | `/firmware-updates/{bridgeId}` → `{id, version, changeLog}` |
| **OTA auslösen** | POST | `/firmware-updates/{bridgeId}/trigger` — Body `{firmwareId}` |
| **Device‑Provisioning** | POST | `/device-provisionings` — Body `{bridgeId}` |
| **BT‑Challenge (TEA‑Key)** | POST | `/bluetooth-challenges` — Body `{"btChallengeId":"OBI-XXXXXX"}` → `{key}` |
| Historie Zähler/Intervall | GET | `/historical-data/{bridge}/{device}/meter` · `/{interval}` |
| Analytics | GET | `/analytics/{a}/{b}/forecast` · `/{c}/standby` |
| Energy‑Consumers | GET/POST/PUT/DELETE | `/energy-consumers/{id}` |

## Woher die Geräte‑Secrets kommen
- **`/bluetooth-challenges`** gibt den **16‑Byte‑TEA‑Key** des Geräts zurück, das per
  `btChallengeId = OBI-XXXXXX` (sein BLE‑Advertising‑Name) benannt wird. In der Praxis braucht das nur ein
  gültiges OBI‑Login **plus den BLE‑Namen** — der Endpunkt prüft **nicht**, ob das Gerät auf deinem Konto
  registriert ist (schwache Autorisierung, siehe [03-security.md](03-security.md)). Das ist der
  „Cloud‑Weg" in [04-eigene-cloud.md](04-eigene-cloud.md#schritt-0--tea-key-des-geräts-holen).
- **`/device-provisionings`** liefert die AWS‑IoT‑Fleet‑Provisioning‑Credentials, die die App ans Gerät
  pusht: `{caPem, certificatePem, privateKey, clusterEndpointUri, provisioningTemplateName}`. Für die
  eigene Cloud erzeugst du die stattdessen selbst mit `gen_certs.py`.

## Was das Gateway selbst published (MQTT über TLS 8883)
Telemetrie und OTA laufen über MQTT‑Topics (siehe [03-firmware-layout.md](03-firmware-layout.md)):
`$aws/rules/EnergyTrackingBridge[Heartbeat]/<UUID>/state`,
`$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state` und die `…/ota/firmware-*`‑Topics.
Fleet‑Provisioning nutzt `$aws/certificates/create/json` und
`$aws/provisioning-templates/<template>/provision/json`.

## Telemetrie‑Payloads (dekodiert — an echtem Gerät bestätigt)
Alle Telemetrie ist **JSON** (nicht binär). Drei Formen:

**Bridge‑State / Heartbeat** → `$aws/rules/EnergyTrackingBridge[Heartbeat]/<UUID>/state`
(`mqtt_build_bridge_state`):
```json
{ "uuid": "<bridge>", "hardware_version": "6.0.0", "firmware_version": "1.0.1", "ota": null,
  "paired_sensor": [ { "sensor": null | {
      "uuid": "<sensor>", "hardware_version": "6.0.0", "firmware_version": "32.0.0",
      "online": true, "battery": 100, "ota": null, "sensor_upload_interval": 300 } } ] }
```
`sensor` ist `null`, bis ein Reader gebunden **und** meldend ist.

> **Schema‑Hinweis — Firmware 1.2.x (`schema_version: 2`).** Der `1.2.x`‑Zweig benennt/erweitert das: der
> Bridge‑State bekommt `"schema_version": 2` und ein `"ota"`‑Status‑Feld, `paired_sensor[].sensor` wird zu
> `paired_devices[]` mit einem `"device"`‑Typ (`"meter"`, und 1.2.x ergänzt Smart‑**Outlet**‑Support),
> `sensor_upload_interval` wird zu `upload_interval`, und das Gerät abonniert Wildcard‑Topics
> (`sensor/+/…`, `outlet/+/control`, `outlet/+/upload-interval-change-request`). Live bestätigt nach dem
> Flashen einer Bridge auf 1.2.1: sie füllte auch **`negative_energy`** (Einspeisung), das unter 1.0.x
> `null` war. Die `1.0.x`‑Form oben ist, was dieses Repo primär dokumentiert; Feldnamen auf `1.2.x`
> gegenprüfen.

#### Firmware‑1.2.x‑Telemetrie, reversed (schema_version 2)
Dekodiert aus den 1.2.1‑Buildern `mqtt_build_bridge_state_v12`, `mqtt_build_outlet_state` und dem Sensor‑
Publisher. Der Geräte‑Typ in `paired_devices[].device` ist `"meter"` (Typ `0x10`) oder `"outlet"` (`0x11`).

**Bridge‑Heartbeat** → `$aws/rules/EnergyTrackingBridge/<UUID>/state` (cmd 3) und `…Heartbeat/…/state` (cmd 4):
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
Ein `meter` trägt `battery`; ein `outlet` trägt `relay` (und `battery: null`). `online` = Last‑Seen‑Zähler ≥ 2.

**Smart‑Outlet‑State** → `$aws/rules/EnergyTrackingOutlet/bridge/<UUID>/outlet/<UUID>/state` (cmd 6) und
`dt/…/outlet/<UUID>/state/live` (cmd 8) — `mqtt_build_outlet_state`:
```json
{ "uuid": "<outlet>", "bridge_uuid": "<bridge>", "hardware_version": "…", "firmware_version": "…",
  "online": true, "timestamp": 1700000000, "rssi": -70,
  "relay": "on" | "off",
  "voltage": 230000,        // mV
  "current": 512,           // mA
  "power": 118,             // W (signed)
  "energy": 12345,          // kumulativer Bezug
  "negative_energy": 0 }    // kumulative Einspeisung
```
Das Reader/Meter‑Energie‑Payload ist gegenüber 1.0.x unverändert, außer `sensor_upload_interval`→
`upload_interval` im Bridge‑State; die Werte kommen aus `processMeterData` (`pos power / neg power / power /
interval / time delta`, plus `softver, hardver, voltage, infrared, lowpower`).

**Sensor‑Energie (die Zählerwerte)** → `$aws/rules/EnergyTrackingSensor/bridge/<UUID>/sensor/<UUID>/state`
(periodisch, alle `sensor_upload_interval` s) und `dt/…/sensor/<UUID>/state/live` (live; jeder 6. Frame ist
„full") — `mqtt_build_sensor_energy`:
```json
{ "uuid": "<sensor>", "bridge_uuid": "<bridge>", "hardware_version": "6.0.0",
  "firmware_version": "32.0.0", "online": true,
  "timestamp": 1700000000,        // Unix-Epoch (Sekunden)
  "rssi": -83,                    // LoRa-Signal vom Reader
  "battery": 100,
  "energy": 12345678,             // kumulativer Bezug   (OBIS 1.8.0), roher Zähler-Register-Wert
  "negative_energy": null,        // kumulative Einspeisung (OBIS 2.8.0), null = keine Daten
  "power": null }                 // Momentanleistung    (OBIS 16.7.0), null = nicht in diesem Frame
```
`energy`/`negative_energy`/`power` werden **`null`** bei Wert `0x7FFFFFFF` (keine Daten) oder inaktivem
Reader; `power` kommt meist in den „full/live"‑Frames. Die **Einheit** von `energy` bestimmst du, indem du
den Rohwert mit dem 1.8.0‑Stand deines Zählers vergleichst (Roh‑Register, je nach Zähler Wh oder 0.001 kWh).
Beispielwerte oben sind Platzhalter/illustrativ.

## Downlink‑Kommandos (Cloud → Gerät)
Das Gerät **abonniert** auch Command‑Topics und reagiert auf das dort gepublishte JSON. Vollständig
reversed und verifiziert ist die **Änderung des Reader‑Upload‑Intervalls** (wie oft die Zählerwerte
gepusht werden — die „300 s").

**Upload‑Intervall ändern** — die Cloud published auf das Topic, das das Gerät abonniert:
```
cmd/energy-tracking/bridge/<BRIDGE-UUID>/sensor/<SENSOR-UUID>/upload-interval-change-request
```
```json
{ "sensor_upload_interval": 60, "session_id": 1 }
```
- `sensor_upload_interval` — neue Periode in **Sekunden**, als **u16** gelesen (1..65535). Dasselbe Feld
  taucht im Bridge‑State unter `paired_sensor[].sensor.sensor_upload_interval` auf.
- `session_id` — eine frei wählbare Ganzzahl; das Gerät speichert und spiegelt sie zurück (Korrelation).

Firmware‑Handler: `mqtt_set_upload_interval` (Protokoll 2, cmd 6) parst das JSON, ruft intern
`set_upload_interval(sensor_upload_interval)`, speichert `session_id` und **antwortet** auf
`…/upload-interval-change-response`:
```json
{ "uuid": "<sensor>", "bridge_uuid": "<bridge>", "sensor_upload_interval": 60,
  "session_id": 1, "timestamp": 1700000000 }
```
Das neue Intervall erscheint danach im nächsten Bridge‑State‑Heartbeat.

**Senden vom lokalen Broker.** Da unser minimaler Broker nicht zwischen Clients routet, injiziert der
Broker selbst das Kommando: `mqtts_server.py --set-interval <sekunden>` wartet, bis das Gerät sein
`…/upload-interval-change-request`‑Topic abonniert, und published das Payload direkt zurück (siehe
[04-eigene-cloud.md](04-eigene-cloud.md)):
```bash
python mqtts_server.py --set-interval 60          # 60 s statt 300 anfordern
```
Im Log erscheint `-> SET sensor_upload_interval = 60s`, danach ein `…/upload-interval-change-response`‑
PUBLISH des Geräts als Bestätigung (live verifiziert: das Gerät antwortet mit exakt obigem Response‑Schema,
und der nächste Bridge‑State‑Heartbeat trägt `"sensor_upload_interval": 60`).

> **Firmware 1.2.x (Multi‑Sensor).** Die `1.2.x`‑Bridge unterstützt mehrere Sensoren (und Smart‑Outlets),
> daher änderte sich der Befehl: das Gerät **abonniert mit Wildcard** (`cmd/…/sensor/+/upload-interval-change-request`)
> und der Request **muss die Ziel‑Sensor‑`uuid` enthalten** — auf das *konkrete*
> `…/sensor/<SENSOR-UUID>/…`‑Topic publishen mit `{"uuid":"<SENSOR-UUID>","sensor_upload_interval":15,"session_id":15}`
> (Handler `mqtt_set_sensor_interval_v12` / `dealMeterIntervalCommand`; ohne `uuid` loggt er
> `get uuid failed`). Die Response nutzt das umbenannte Feld: `{uuid, device:"meter", bridge_uuid,
> session_id, upload_interval, timestamp}`. Es gibt einen parallelen `…/outlet/…`‑Befehl für Smart‑Outlets
> mit `upload_interval`. `mqtts_server.py --set-interval` erledigt das alles: es lernt die Sensor‑uuid aus
> der Telemetrie, adressiert das konkrete Topic und ergänzt `uuid`. Live verifiziert: eine 1.2.1‑Bridge
> übernahm `15 s` und echote `upload_interval: 15`.

> **Timing (live verifiziert):** Ack und Heartbeat‑Update sind sofort da. `sensor_upload_interval` steuert,
> **wie oft das Gateway** `EnergyTrackingSensor/…/state` published — es re‑published seinen zuletzt gecachten
> Reader‑Wert in diesem Takt. Die **LoRa‑Report‑Rate des Readers ist davon getrennt** (~300 s auf der
> getesteten Einheit), d. h. der `energy`‑*Wert* ändert sich erst beim nächsten Reader‑Report; dazwischen
> sendet das Gateway denselben Wert nur häufiger. Direkt nach der Änderung holt das Gateway verpasste
> Fenster nach — man sieht evtl. kurz mehrere identische Werte, danach gleichmäßig im neuen Intervall.

### OTA‑Firmware‑Update (Image über MQTT einspielen) <a id="ota"></a>
Vollständig reversed und verifiziert (byte‑genau gegen einen echten Cloud‑Mitschnitt + End‑to‑End‑
Selbsttest). Das Gateway‑Selbst‑Update ist **unsigniert** — nur ein Integritäts‑Hash — d. h. ein Gerät an
**deiner eigenen Cloud** flasht jedes Image, das du auslieferst. Das ist der Custom‑Firmware‑Weg. Vier
Topics unter `…/bridge/<UUID>/ota/`:

| Richtung | Topic | Payload |
|---|---|---|
| Cloud → Gerät | `cmd/…/ota/firmware-update-request` | **23 Bytes** binär: `[maj][min][patch] + total_len(u32 BE) + md5(16)` |
| Gerät → Cloud | `cmd/…/ota/firmware-update-response` | echot dieselben 23 Bytes (Ack) |
| Gerät → Cloud | `dt/…/ota/firmware-data-request` | JSON `{"uuid","firmware_version":"X.Y.Z","offset":N}` |
| Cloud → Gerät | `dt/…/ota/firmware-data-response` | **9‑Byte‑Header** `[maj][min][patch] + offset(u32 BE) + len(u16 BE)` + bis zu **512** Image‑Bytes |

Ablauf (Firmware‑Handler `mqtt_ota_file_info`, `mqtt_ota_data`, Requester `sub_4200E6AE`):
1. Cloud published den 23‑Byte‑**Offer**. Die Version ist kosmetisch (das Gerät verlangt nicht, dass sie
   neuer ist); `total_len` **muss** der Image‑Größe entsprechen — daran erkennt das Gerät das Ende.
2. Gerät echot ihn zurück (Ack), macht `esp_ota_begin` und **zieht** dann: es published einen Data‑Request
   für `offset:0`, nach jedem geschriebenen Chunk den nächsten `offset` (`+512`). Kommt ein Chunk nicht
   binnen ~10 s, wird er neu angefragt (max. 5 Versuche, dann Abbruch).
3. Cloud beantwortet jeden Request mit `firmware[offset:offset+512]` hinter dem 9‑Byte‑Header. Die
   Versions‑Bytes müssen zum Offer passen (3‑Byte‑`memcmp`), und der `offset` muss der aktuellen
   Schreibposition entsprechen.
4. Bei `offset >= total_len` macht das Gerät `esp_ota_end` → `esp_ota_set_boot_partition` → **Reboot** ins
   neue Image. Ein abgebrochener Transfer schaltet die Boot‑Partition nie um — vorzeitig stoppen ist sicher.

Echter Offer aus einem Mitschnitt: `01000200173d74d94189299932921089a2e041ed8e145c` → v1.0.2, `0x00173D74`
= 1 522 548 Bytes, dann die 16‑Byte‑md5. Das Image beginnt mit `0xE9` (ESP32‑App‑Magic).

**Vom lokalen Broker ausliefern:** `mqtts_server.py --ota-firmware fw.bin` schiebt den Offer, sobald das
Gerät subscribed, und beantwortet jeden Data‑Request — siehe
[04-eigene-cloud.md](04-eigene-cloud.md#eigene-firmware-flashen). Ein Stock‑Image zum Flashen (oder
Studieren) holst du dir vom Hersteller mit
[`obi_ota_download.py`](../04-connect-your-own-cloud/tools/obi_ota_download.py) (spielt die Geräteseite).

## Historical‑Data‑Parameter
- `duration`: ISO‑8601‑Intervall, z. B. `2026-07-01T23:00:00Z/PT24H`
- `measures`: `energy`, `negative_energy` (kommagetrennt)

> Alles aus dem Client rekonstruiert, für Interoperabilität mit einem Gerät, das dir gehört. Eigenes Konto,
> eigene Hardware.
