# Firmware‑Images — nicht enthalten (🇩🇪)

> **Aus Sicherheitsgründen werden in diesem Repo keine Vendor‑Firmware‑Binaries verteilt.** Die Images von
> Gateway (ESP32‑C3) und Reader (BAT32G135) sind **SUMEC‑/OBI‑Copyright** und daher entfernt und
> git‑ignoriert (`firmware/**/*.bin`, `*.elf`). Das Repo enthält nur *Doku und Tools* — das Image bringst du
> von einem Gerät mit, das **dir gehört**.

## Ein Image bekommen (deins)
- **Aus der eigenen Cloud ziehen** mit
  [`../04-connect-your-own-cloud/tools/obi_ota_download.py`](../04-connect-your-own-cloud/tools/obi_ota_download.py):
  spielt den OTA‑Client des Geräts nach und speichert das App‑Image, das der Hersteller für *deine* Bridge
  ausliefert (braucht ein Provisioning‑Cert für die UUID deiner Bridge — siehe
  [03-cloud-api.md](03-cloud-api.md)).
- **Vom eigenen Gerät dumpen, sobald die Custom‑Firmware läuft**: die Web‑Debug‑Seite liest den ganzen
  SPI‑Flash (beide OTA‑Slots → laufende *und* vorherige Version). Kein Cert, kein esptool nötig — Schritte
  + Partitions‑Splitter in [04-flash-dumpen.md](04-flash-dumpen.md).
- Oder ein bereits vorhandenes Dump. Der ROM‑Download‑Modus ist auf Serien‑Geräten per eFuse gesperrt
  (siehe [03-firmware-layout.md](03-firmware-layout.md)), daher ist kein UART/JTAG‑Auslesen möglich; der
  OTA‑Weg oben ist der praktikable.

Geräte‑spezifische Secrets liegen ohnehin nicht im App‑Image — TEA‑Key und Cloud‑Certs stehen zur Laufzeit
in den NVS/Daten‑Partitionen, nicht im Code‑Image.

## ESP32‑C3‑Gateway‑Image laden (RISC‑V)
Jedes Segment auf seine Load‑Adresse mappen (ein flacher Load disassembliert nicht). Wrapper‑Ansatz und
Segment‑Map: [03-firmware-layout.md](03-firmware-layout.md).

## BAT32G135‑Reader‑Image laden (ARM Cortex‑M0+)
`reader/cortexm_setup.py` ist enthalten — ein IDA‑Script, das die exakte BAT32G135‑Memory‑Map ergänzt und
die Vektortabelle benennt. Schritte, sobald du dein eigenes `reader.bin`/`reader.elf` hast:

1. Image in IDA laden als **ARM Little‑endian, Thumb (Cortex‑M0+)**, Basis `0x0` (ein ELF‑Wrapper wird
   automatisch erkannt; ein rohes `.bin` braucht manuell Prozessor **ARMv6‑M / Cortex‑M0+**).
2. `reader/cortexm_setup.py` ausführen (File → Script file): fügt `DATAFLASH`/`SRAM`/`PERIPH`/`SCS`‑Segmente
   hinzu, erzwingt Thumb und benennt die Exception/IRQ‑Vektoren.

Memory‑Map (aus dem Datenblatt): Code‑Flash 64 KB @`0x0`, Data‑Flash 1.5 KB @`0x500000`, SRAM 8 KB
@`0x20000000`, Peripherie `0x40000000`–`0x4005FFFF`, SCS `0xE0000000`. Der Reader ist ein SUMEC‑Zähler‑Reader
(OBIS/IEC‑62056) — siehe [02-hardware.md](02-hardware.md). Verschiedene Gateway‑Versionen tragen
verschiedene Reader‑Builds; `1.2.x`‑Reader sind größer (BAT32‑Variante mit mehr Flash).
