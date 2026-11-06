# v57/v90-Reader: Live-Werte im Sekundentakt (experimentell) (🇩🇪)

> ➡️ **English version: [../README.md](../README.md)**

> ✅ **An einem Reader verifiziert** (06.11.2026): `reader_modded_live_91mock_v92_EMHeHZP.bin` an einem
> per USB versorgten Reader (hardver 6) an einem Landis+Gyr E320 (SML-Push jede Sekunde). Der Reader lief
> vorher mit der Legacy-v32, über den Legacy-Bootloader-Pfad des Gateways ging er ohne Neu-Kopplung direkt
> auf v57. Gemessen über 61 Energie-Frames am Gateway: Median-Abstand **1,00 s** (max. 2,0 s, jeder Frame
> bestätigt); Original-v57 schafft ~10 s. Die anderen sechs Varianten tragen dieselben drei Patches, sind
> aber nicht einzeln getestet.

Eine Live-Variante jedes v57-basierten Images in diesem Ordner (`reader_stock_v57.bin` und die sechs
`reader_modded_89mock_v90*.bin`). Im Live-Modus (Intervall 1–2 s) sollte der Reader an einem Zähler mit
sekündlichem SML-Push etwa einen Wert pro Sekunde senden. Das Original-v57 schafft etwa einen alle
**10 s**. Andere Intervalle verhalten sich exakt wie bisher.

## Was v57 im Live-Modus bremst

1. **Lese-Zyklus.** Es ist derselbe Zyklus, den der v32-Fix behebt (siehe
   [../../legacy_v32/de/README.md](../../legacy_v32/de/README.md)). Der Reader nimmt ein Telegramm und
   wartet dann in Zustand `0x168`, bis `(jetzt_s - start_s) > period` gilt. Die Prüfung erledigt
   `sub_B7C8`, die Periode ist das Upload-Intervall, also kostet die Pause auch im Live-Modus 2–3 s.
2. **LoRa-Fenster.** Der Reader sendet nur in einem ~100-ms-Fenster, das er **9950 ms** nach dem
   zuletzt gehörten Gateway-Beacon öffnet (`reader_lora_timing_sm`, Konstante bei `0xB9C0`, siehe
   [../../../de/03-lora-protokoll.md](../../../de/03-lora-protokoll.md)). Das Gateway sendet den Beacon
   mit 1 Hz, der Reader hört also nur jeden zehnten.

## Der Patch

Drei 4-Byte-Stellen springen per BL in einen 64-Byte-Block ([../live.S](../live.S)). Der Block ist
positionsunabhängig und wird ans Image angehängt:

| Stelle | Original | Hook |
|---|---|---|
| `0xD11E` | `bl sub_B7C8` (Zyklus-Pause) | `entry_cycle_due`: sofort fällig bei Periode ≤ 2, sonst Sprung in die Original-Prüfung |
| `0xB942` | `ldr r1,=9950; movs r0,#0` (nächstes Fenster) | `entry_window_base`: 950 ms bei Intervall 1–2, sonst 9950 |
| `0xB906` | `ldr r1,=9950; adds r0,r0,r1` (Fenster nach Fehlversuch) | `entry_window_retry`: dieselbe Wahl plus Wiederholungs-Offset |

- Das Intervall kommt aus `0x20000114`. Dort trägt der Energie-ACK des Gateways den Wert ein (Getter
  `sub_79E8`). Bei Intervall 1–2 hört der Reader **jeden** Beacon (Fenster bei +950 ms, Beacon bei
  +1000 ms). Ein noch nicht gesetztes Intervall 0 behält das Original-Fenster.
- Der Block ruft die Original-Prüfung über ihre absolute Adresse auf. Die Images laufen ab
  **`0x4800`**: Die Repo-Nummerierung ist Datei-Offset + `0x4000`, also liegt `sub_B7C8` zur Laufzeit
  bei `0xBFC8`. `build.sh` bricht ab, falls der Block jemals eine Relokation bräuchte.
- softver 57/89 → **91**, Dateinamen `…_live_91mock_v92…`. Zurück auf eine v90-Datei geht weiterhin,
  denn das Gateway bewirbt 90 und der Reader meldet 91.
- Größe: Das größte Live-Image endet bei Laufzeit `0xF780`, weit unter `0xFC00`. Der v32-Test hat
  gezeigt, dass der Bootloader Images ablehnt, die bis in diese letzte Flash-Seite reichen.

## Bauen

```bash
cd reader_firmware_mod/live_v57
./build.sh                          # braucht arm-none-eabi-gcc
python3 patch_live.py --all         # alle sieben Images -> *_live_91mock_v92*.bin
python3 patch_live.py ../reader_modded_89mock_v90_DWSB20_2TH.bin   # nur eins
```

`patch_live.py` nimmt nur die exakt eingecheckten Images an (SHA-256-Liste im Skript). Werden die
v90-Releases mit anderen Bytes neu gebaut, muss diese Liste angepasst werden. Die Live-Images sind nicht
eingecheckt; die CI baut sie, und `patch_live.py` erzeugt sie byte-gleich neu.

Flashen wie gewohnt (Dashboard → Reader-Karte → **Firmware flashen**), danach das Intervall des
Readers auf **Live** stellen.

## Einschränkungen

- **Nur mit USB-Versorgung.** Im Live-Modus wacht der Reader zu jedem Beacon auf, liest den Zähler
  laufend und funkt etwa einmal pro Sekunde. Mit Batterie den Live-Modus nicht nutzen. Andere
  Intervalle sind unverändert.
- **Ein Live-Reader pro Gateway.** v57-Reader senden direkt vor dem Beacon. Mehrere Live-Reader
  teilten sich jede Sekunde denselben ~50-ms-Slot und stießen zusammen.
- **Sendezeit.** Bei SF7 belegt ein 1-Hz-Reader grob 2 % der 10 % Duty-Cycle des Teilbands, bei SF9
  mehr.
