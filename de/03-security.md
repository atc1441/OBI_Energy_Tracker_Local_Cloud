# Sicherheits‑Notizen (🇩🇪)

Sachliche Schwachstellen aus dem Reversing, damit **Besitzer** ihre Geräte einschätzen und selbst hosten
können.

## 1. TEA‑Key über UART les-/schreibbar (Klartext)
Der UART0‑Config‑Kanal (`C5 5C …`) hat **keine Authentifizierung**. `cmd 49` liefert den 16‑Byte‑TEA‑Key,
`cmd 48` setzt ihn, `cmd 58/59` schreiben/lesen WLAN‑Daten — alles im Klartext. Wer an die Konsolen‑Pins
(GPIO20/21) kommt, besitzt den BLE‑Kanal und das WLAN. Zugleich der vorgesehene Self‑Host‑Einstieg. Siehe
[03-uart-config.md](03-uart-config.md).

## 1b. TEA‑Key per BLE‑Namen aus der Cloud abrufbar (schwache Autorisierung)
Der Hersteller‑Endpunkt `POST /bluetooth-challenges` gibt den **16‑Byte‑TEA‑Key** eines Geräts allein anhand
seines BLE‑Advertising‑Namens (`btChallengeId = OBI-XXXXXX`) und **irgendeinem gültigen OBI‑Login** zurück —
er prüft **nicht**, ob das anfragende Konto das Gerät besitzt oder je eingebunden hat. Da der `OBI-XXXXXX`‑
Name auf dem Aufkleber steht und in jedem BLE‑Advertisement gesendet wird, kann jeder, der das Gerät sieht
(oder seinen Namen kennt) und ein OBI‑Konto hat, den Key des BLE‑Steuerkanals holen. Zusammen mit #1 ist der
Per‑Device‑TEA‑Key faktisch wenig geheim. Für Besitzer: bevorzugt den UART‑Weg nutzen und den BLE‑Key als
nicht‑geheim behandeln. Siehe [03-cloud-api.md](03-cloud-api.md#woher-die-geräte-secrets-kommen).

## 2. LoRa‑Link ohne echte Krypto
Frames sind mit **1‑Byte‑XOR** obfuskiert, dessen Key die Byte‑Summe des Klartext‑Handles ist — aus jedem
mitgeschnittenen Frame ableitbar. Der ECDH (cmd 32) gated den Datenfluss, sein Secret wird nie genutzt.
Folge: LoRa‑Traffic lässt sich lesen, fälschen und einspeisen (z. B. Fake‑Zählerwerte, oder die
gespeicherte Reader‑Firmware per ungebundenem cmd 21 ziehen). Siehe [03-lora-protokoll.md](03-lora-protokoll.md).

## 3. BLE‑Fragment‑Reassemblierung — Heap‑Overflow (Teil‑Fund)
`ble_reassemble_frags` alloziert beim ersten Fragment einen **festen 5120‑Byte**‑Puffer und hängt jedes
weitere Fragment per `memcpy(buf + 4 + total, frag, len)` an, `total += len`. Fragmente werden nur auf
**Reihenfolge** geprüft (Paketnummer passt, index = vorher+1) — die **Gesamtlänge wird nie gegen die 5120
Bytes geprüft**.
```
erstes Fragment -> malloc(5120)
jedes Fragment  -> memcpy(buf + 4 + total, frag, len); total += len   # keine Grenze für total
```
Eine Nachricht in ~30+ Fragmenten à ~173 Byte überläuft den Puffer (Heap‑Overflow). Das index‑Feld ist ein
Byte → bis 256 Fragmente (~44 KB) bei fester 5120‑Byte‑Allokation.

**Erreichbarkeit / Caveat:** Die Reassemblierung läuft **nach der TEA‑Entschlüsselung** — man braucht also
einen gültigen TEA‑authentifizierten Fragment‑Stream, d. h. den TEA‑Key des Geräts (der selbst holbar ist,
siehe #1). Ein *authentifizierter* Heap‑Overflow. Hier **nicht weaponized/getestet** — als Teil‑Fund
dokumentiert; Exploitierbarkeit (Heap‑Layout, Allocator, was nach dem Puffer liegt) ist noch offen.

## 4. eFuse‑USER_DATA‑Key im Boot‑Log
Beim Boot liest die Firmware einen 16‑Byte‑eFuse‑USER_DATA‑Wert und loggt ihn ("Key content") auf UART
(INFO‑Level). Boot‑Log mitschneiden kann ihn zeigen. Siehe [03-uart-config.md](03-uart-config.md).

## 5. Gesperrter Bootloader (Hardening, zur Einordnung)
eFuses deaktivieren JTAG und ROM‑Download‑Modus auf provisionierten Geräten → kein Reflash über UART/JTAG.
Custom‑Firmware muss über den Cloud‑OTA‑Weg. Siehe [03-firmware-layout.md](03-firmware-layout.md).

---
Diese Notizen dienen dem Absichern/Self‑Hosten eigener Hardware.
