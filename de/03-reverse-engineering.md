# 03 · Reverse Engineering (🇩🇪)

Protokoll‑Doku, rekonstruiert aus der ESP32‑C3‑Gateway‑Firmware (RISC‑V) und der BAT32G135‑Reader‑Firmware
(ARM Cortex‑M0+).

## Seiten
| Datei | Inhalt |
|---|---|
| [03-firmware-layout.md](03-firmware-layout.md) | Image‑Struktur, Segmente, eingebettete Reader‑FW, vsocket/Dispatch, OTA, Boot‑Hardening |
| [03-ble-protokoll.md](03-ble-protokoll.md) | GATT ABF0/1/2, TEA‑Cipher, Fragmentierung, JSON‑Commands |
| [03-lora-protokoll.md](03-lora-protokoll.md) | 868‑MHz‑Frame‑Format, Command‑Satz, Richtungen, XOR, ECDH‑Gate, Energie‑Payloads |
| [03-uart-config.md](03-uart-config.md) | `C5 5C` Klartext‑Config‑Kanal (TEA‑Key / WLAN lesen+schreiben) |
| [03-cloud-api.md](03-cloud-api.md) | Die Hersteller‑Cloud‑REST‑API (Auth, Environments, Endpunkte, woher Key/Certs kommen) |
| [03-security.md](03-security.md) | Gebündelte Schwachstellen inkl. BLE‑Reassembly‑Heap‑Overflow (Teil‑Fund) |

## Vorgehen (zum Nachvollziehen)
Das ESP‑IDF‑Gateway‑Image wurde in eine RISC‑V‑ELF verpackt (Segmente an ihre Load‑Adressen) und in IDA
geladen; ~1000 Funktionen wurden ab den `__func__`‑Strings des ESP‑IDF‑Loggers benannt, dann Subsystem für
Subsystem. Das Reader‑Image ist ein rohes Cortex‑M0+‑Binary, hinter die ESP‑Segmente angehängt — siehe
[../firmware/README.md](../firmware/README.md) für die IDA‑fertige ELF + Setup‑Script.

## Kernfakten (Kurzfassung)
- **BLE**‑Steuerkanal: **klassisches TEA** (32 Runden, Delta `0x9E3779B9`, ECB), ein 16‑Byte‑Key pro Gerät,
  JSON‑Payload, 173‑Byte‑Fragmentierung. Key wird provisioniert, in NVS gespeichert.
- **LoRa**‑Link: geframed, über 6‑Bit‑Command verteilt; „Verschlüsselung" ist **1‑Byte‑XOR**, dessen
  Schlüssel die Byte‑Summe des Klartext‑Handles ist. Ein ECDH‑P‑256 gated den Datenfluss, das Secret bleibt
  ungenutzt.
- **UART0** bietet ein **Klartext**‑Config‑Protokoll, das TEA‑Key, WLAN‑Daten u. a. lesen/schreiben kann —
  der praktische Einstieg fürs Self‑Hosting.
- **OTA**: Gateway‑Selbst‑Update via MQTT (esp_ota, Dual‑Partition); die Reader‑Firmware ist **im
  Gateway‑Image eingebettet** und wird per LoRa an die Reader ausgeliefert.
- **Boot‑Hardening**: eFuses deaktivieren JTAG + ROM‑Download‑Modus (gesperrter Bootloader).

Alle Beispiel‑Schlüssel sind Platzhalter.
