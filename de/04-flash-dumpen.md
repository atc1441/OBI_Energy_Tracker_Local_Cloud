# Den ganzen Flash vom eigenen Gerät dumpen (und die OTA‑Partitionen einzeln herausziehen) (🇩🇪)

Der [Cloud‑OTA‑Download](../04-connect-your-own-cloud/tools/obi_ota_download.py) holt das *App‑Image, das
der Hersteller ausliefert*, braucht aber ein Provisioning‑Cert und ist fummelig. Sobald die **Custom‑
Firmware aus diesem Repo läuft**, gibt es einen zweiten, viel einfacheren Weg: die **Web‑Debug‑Seite der
Firmware kann den SPI‑Flash des Geräts selbst auslesen** — den ganzen Chip, inklusive *beider* OTA‑App‑
Slots. Damit bekommst du:

- die **aktuell laufende** Firmware,
- die **vorherige Version**, die noch im anderen OTA‑Slot liegt (das, was das letzte Update ersetzt hat), und
- Bootloader, Partitionstabelle, NVS und Daten‑Partitionen.

Kein esptool, kein UART, kein Cert — nur ein Browser. (Die **Stock**‑Firmware hat keine solche Seite — das
geht erst *nach* dem Flashen der Custom‑Firmware. Das Stock‑Image selbst holt man den
[Cloud‑Weg](04-eigene-cloud.md#eigene-firmware-flashen).)

> ⚠️ Du bist am rohen Flash. **Lesen** ist harmlos. Nutze auf der Debug‑Seite **kein** Write / Erase / den
> Hex‑Editor, außer du kennst den exakten Offset — ein falscher Write brickt das Gerät. Diese Anleitung
> liest nur.

---

## 1. Flash‑Debug‑Seite öffnen

1. Sicherstellen, dass die Bridge in deinem WLAN ist und die Web‑UI erreichbar (`http://<geräte-ip>/`).
2. Oben rechts **🐞 Debug** klicken, oder direkt `http://<geräte-ip>/debug` aufrufen.

Die Leiste oben hat ein Adressfeld, ein **Partitions‑Dropdown**, **Save .bin**, **Dump full flash** und
**eFuses**.

## 2. Zuerst die Partitionstabelle lesen

Das **— partition —**‑Dropdown öffnen. Es listet jede Partition auf *diesem* Gerät mit Label, Offset und
Größe, z. B.:

```
nvs      @0x9000 · 20K
otadata  @0xe000 · 8K
app0     @0x10000 · 1920K      <- OTA-Slot 0  (Firmware)
app1     @0x1f0000 · 1920K     <- OTA-Slot 1  (Firmware)
spiffs   @0x3d0000 · 128K
```

Diese Zahlen aufschreiben — danach wird geteilt. (Offsets/Größen sind pro Gerät fix; die zwei App‑Slots
heißen evtl. `app0`/`app1` oder `ota_0`/`ota_1`.) Dieselbe Tabelle gibt es auch über das UART‑Kommando
`info` oder per `GET http://<geräte-ip>/api/flash/info`.

Welcher App‑Slot *läuft* vs. *vorherig*: der laufende ist die Boot‑Partition, der andere hält das Image von
vor dem letzten OTA. Wenn unklar, einfach beide ziehen — dann hast du beide Versionen.

---

## Variante A — eine Partition direkt ziehen (am einfachsten, kein Splitten)

Pro OTA‑Slot einmal machen und fertig — die Debug‑Seite speichert eine einzelne Partition für dich:

1. Im Partitions‑Dropdown den gewünschten Slot wählen (z. B. `app0`).
2. **Save .bin** klicken. Es fragt *„Bytes to read & save from 0x…"* und schlägt „bis Flash‑Ende" vor.
   **Ersetze das durch die Partitionsgröße in Bytes**, damit du nur diese eine Partition greifst. Im
   Beispiel oben sind 1920K = `1920 × 1024 = 1966080`.
3. Es lädt `flash_0x<offset>_<size>.bin` herunter — nur diese Partition.

Für `app1` wiederholen, um die zweite (vorherige) Version zu bekommen. Jede Datei ist ein normales
ESP32‑C3‑App‑Image (beginnt mit `0xE9`) — bereit zum erneuten Servieren mit
[`mqtts_server.py --ota-firmware …`](04-eigene-cloud.md#eigene-firmware-flashen), zum Laden in Ghidra/IDA
oder als Backup.

---

## Variante B — ganzen Flash dumpen, dann splitten (was man teilen muss)

Wenn du *alles* auf einmal willst (Backup oder um das Layout zu studieren):

1. Auf `/debug` **Dump full flash** klicken und bestätigen. Es liest den ganzen Chip — Bootloader +
   Partitionstabelle + beide Apps + NVS — und lädt `flash_full_<size>k.bin` (z. B. `flash_full_4096k.bin`
   = 4 MB). **Das ist über WLAN langsam** (ein paar Minuten für 4 MB); den Tab offen lassen, bis der
   Download erscheint.

Diese eine Datei ist das rohe Chip‑Image. Um die **OTA‑Partitionen als getrennte Dateien** zu bekommen,
teilst du sie nach den Offsets aus Schritt 2. Am einfachsten mit dem beiliegenden Splitter, der die
Partitionstabelle *aus dem Dump selbst* liest (keine Offsets tippen nötig):

```bash
# listet die Partitionen und schreibt ota_0.bin, ota_1.bin, nvs.bin, … daneben:
python 06-tools/split_flash_dump.py flash_full_4096k.bin -o out/

# nur die zwei Firmware-Slots:
python 06-tools/split_flash_dump.py flash_full_4096k.bin -o out/ --only app0,app1

# nur die Tabelle ansehen, nichts schreiben:
python 06-tools/split_flash_dump.py flash_full_4096k.bin --list
```

Du bekommst eine `<label>.bin` pro Partition — `app0.bin` / `app1.bin` sind die zwei Firmware‑Versionen.

### Von Hand splitten (dd)

Wenn du das Skript nicht nutzen willst, mit `dd` schneiden — Offset/Größe **in Bytes** aus Schritt 2
(hier dezimal; die Hex‑Offsets umrechnen, z. B. `0x10000 = 65536`, `1920K = 1966080`):

```bash
dd if=flash_full_4096k.bin of=app0.bin bs=1 skip=65536  count=1966080
dd if=flash_full_4096k.bin of=app1.bin bs=1 skip=2031616 count=1966080
```

(`bs=1` ist simpel, aber langsam; schneller mit `bs=4096` und Byte‑Offsets/‑Größen durch 4096 teilen für
`skip`/`count`, da hier jede Partition 4‑KB‑aligned ist.)

---

## Wofür jede Datei gut ist

| Datei | Was es ist | Nutzen |
|---|---|---|
| `app0.bin` / `app1.bin` | volle ESP32‑C3‑App‑Images (`0xE9`‑Magic) | per Cloud‑OTA reflashen, reversen, als Backup halten |
| `nvs.bin` | Non‑Volatile‑Settings | enthält WLAN‑Daten, Cloud‑Certs, TEA‑Key zur Laufzeit (nicht im App‑Image) |
| `flash_full_*.bin` | der rohe 4‑MB‑Chip | komplettes Backup; jederzeit neu splitten |

Zum Laden eines App‑Images in IDA/Ghidra müssen die Segmente noch gemappt werden — siehe
[03-firmware-layout.md](03-firmware-layout.md). Zum Reflashen auf ein Gerät an deiner eigenen Cloud siehe
[Eigene Firmware flashen](04-eigene-cloud.md#eigene-firmware-flashen).

> **Copyright‑Hinweis:** die extrahierten Stock‑Bridge/Reader‑Images sind SUMEC‑/OBI‑Copyright. Das *eigene*
> Gerät für Backup/Studium zu dumpen ist okay; verteile die Hersteller‑Binaries nicht — dieses Repo liefert
> nur Tools und Doku, nie die Images (siehe [firmware.md](firmware.md)).
