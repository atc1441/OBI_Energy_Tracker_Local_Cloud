# Legacy-v32-Reader: Live-Werte im Sekundentakt (🇩🇪)

> ➡️ **English version: [../README.md](../README.md)**

Ein Patch für den **Legacy-Reader** (Cloud-Firmware „1.0.1", softver **32**, im Gateway-Dashboard als
`legacy` markiert). Im Live-Modus sendet der gepatchte Reader **jedes Zähler-Telegramm**, an einem
Zähler mit sekündlichem SML-Push also etwa eines pro Sekunde. Der Original-Reader schafft nur eines
alle **3–4 s**.

| Gemessen an einem Reader (hardver 6) an einem 1-Hz-SML-Zähler | Original v32 | gepatcht |
|---|---|---|
| Energie-Frames am Gateway (`/api/radio`), Median-Abstand | 3,8 s | **1,00 s** |
| Zähler-Telegramme pro Bericht | 1 von 3–4 | jedes |
| Home Assistant über BTHome (Proxy-Scanfenster 300 ms), Median | ~4 s | **1,02 s** |

Live verifiziert am 05.11.2026 (ein Reader per USB versorgt, SML-Telegramm 276 Bytes, 9600 Baud).

## Warum der Original-Reader langsam ist

Die Zähler-Lese-Logik des Readers (Repo-Nummerierung `0xD624`, Zustands-Struct `0x200001B0`) arbeitet in
Zyklen. Sie schaltet den Zähler-UART ein, nimmt **ein** Telegramm und schaltet den UART wieder ab. Danach
wartet sie in Zustand `0x114`, bis `(jetzt_s - start_s) > period` gilt. `sub_C074` prüft das in ganzen
Sekunden. `period` ist das **Upload-Intervall** (`S[8]`, der Wert aus dem Energie-ACK des Gateways). Das
Preset „Live" des Gateways kommt als Intervall 2 an (`2 × b[1]`, siehe den Legacy-ACK in
`open_obi_energy_meter/src/main.cpp`). Allein die Pause dauert also 3 s, danach wartet der Reader noch
aufs nächste Telegramm. Ergebnis: 3–4 s pro Wert, obwohl der Reader am USB nie schläft und der Zähler
jede Sekunde sendet.

Gefunden mit Diagnose-Builds. Sie schreiben Zähler in die fünf ungenutzten Füllbytes (19–23) jedes
Energie-Frames, und das Radio-Log des Gateways zeigt sie entschlüsselt. Pro Bericht kam genau ein
Telegramm (276 Bytes) an, nichts wurde verworfen, der Reader schlief nie.

## Der Patch

Ein Aufruf wird umgeleitet: `bl sub_C074` bei `0xD7DE` → `entry_cycle_due` ([../live.S](../live.S)). Bei
`period <= 2` meldet der Hook sofort „fällig", und der nächste Lese-Zyklus startet direkt nach dem
vorigen Telegramm. Bei längeren Intervallen ruft er die Original-Prüfung auf, Verhalten und
Stromsparen bleiben unverändert.

- **Kein Platz zum Anhängen.** Der Bootloader lehnt ein Image ab, das bis in die letzte Flash-Seite
  reicht. Ein Test-Image mit Ende bei Laufzeit `0xFC68` wurde immer wieder neu geladen und nie
  geschrieben. Das Original endet bei `0xFBC4`, 60 Bytes unter `0xFC00`. Der 16-Byte-Hook liegt
  deshalb in totem Code: uECCs `vli_mmod_fast_secp256k1` (`0xE6EC`–`0xE773`). Nur das Kurven-Struct von
  secp256k1 zeigt darauf, und dieses Struct verwendet niemand, denn der Reader nutzt nur secp256r1
  (Getter bei `0xDD54`). Das Image behält exakt die Originalgröße.
- **Version.** softver 32 → **92** (Frame-Byte bei `0xD244` und die Eigene-Version-Prüfung im
  ACK-Handler bei `0xAFE8`), der Dateiname endet auf `…v93`. So steht die „gleiche Version"-Prüfung des
  Gateways in keiner Richtung im Weg, auch das Zurückflashen des Originals (`…v32`) funktioniert.
- **Adress-Nummerierung.** Die Adressen folgen der Repo-Nummerierung (Datei-Offset + `0x4000`).
  Tatsächlich läuft das Image ab **`0x4800`**: Die Vektortabelle liegt bei Datei-Offset 0, der
  Reset-Handler bei Datei-Offset `0xD8`. Relative BLs stört das nicht, absolute Code-Adressen
  (Funktionszeiger) in dieser Nummerierung liegen aber `0x800` daneben.

## Bauen und flashen

Hier liegt keine Hersteller-Binary. Du brauchst dein eigenes Original-v32-Image oder etwas, das es
enthält. Eine Stock-App-Partition einer Bridge **1.0.x** oder ein kompletter Bridge-Flash-Dump tragen es
in sich. Direkt nach der Umstellung Stock → Custom liegt im anderen OTA-Slot noch das
Stock-Bridge-Image (siehe Flash-Dump auf der Debug-Seite).

```bash
cd reader_firmware_mod/legacy_v32
./build.sh                                       # braucht arm-none-eabi-gcc
python3 patch_legacy_live.py <image-oder-dump>   # -> reader_legacy_live_92mock_v93.bin
python3 patch_legacy_live.py <image-oder-dump> --extract-only   # -> reader_legacy_stock_v32.bin (Rückweg)
```

Das Skript findet das Image an seiner Vektortabelle, schneidet es am Ende der Scatter-Load-Tabelle ab
und prüft die SHA-256 (`4dfb9c94…b8e2`, 46.020 Bytes). Andere Builds lehnt es ab. Die Ausgabe ist
byte-gleich mit dem live getesteten Image (`10f8ee15…6d48`).

Geflasht wird wie jedes Reader-Image: Dashboard → Reader-Karte → **Firmware flashen**. Danach das
Intervall des Readers auf **Live** (oder 2–3 s) stellen. Das Gateway sendet das als `b[1] = 1`, also
Intervall 2.

> **Heap des Gateways:** Das Gateway hält das ganze Image in einem Puffer. Mit aktivem BTHome kann der
> größte freie Block kleiner als 46 KB sein, dann scheitert der Upload (`{"ok":false}`). BTHome
> abschalten, Gateway neu starten, Reader flashen, BTHome wieder einschalten (nochmal Neustart).

## Einschränkungen

- **USB-Versorgung.** Im Live-Modus bleibt der Zähler-UART an, und der Reader funkt etwa einmal pro
  Sekunde. Am USB kostet das nichts. Mit Batterie den Live-Modus mit diesem Patch meiden (andere
  Intervalle sind unverändert).
- **Sendezeit.** Ein Reader mit 1 Hz belegt grob 2 % der 10 % Duty-Cycle im 869,5-MHz-Teilband.
  v32-Reader senden nach eigenem Takt, mehrere Live-Reader an einem Gateway stoßen also gelegentlich
  zusammen. Nach einem verlorenen ACK sendet der Reader nach ~5 s erneut (seltene Lücken von 5–7 s).
- **Geltungsbereich.** Nur v32. Die v57/v90-Reader haben dieselbe Zyklus-Logik plus ein eigenes
  ~10-s-LoRa-Fenster und brauchen eine eigene Portierung.
