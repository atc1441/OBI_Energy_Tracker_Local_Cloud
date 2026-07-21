# reader_sdk — C-Hook-Framework für die BAT32G135-Reader-Firmware (🇩🇪)

> ➡️ **English version: [../README.md](../README.md)**

Ziel: statt jede Änderung an der (closed-source) Vendor-Firmware per Hand in
Thumb-1-Opcodes zu übersetzen, schreiben wir den Hook-Code in C, kompilieren
ihn mit dem echten `arm-none-eabi-gcc` (Cortex-M0+/ARMv6-M, Thumb-1, kein
FPU) und splicen das Ergebnis in die Firmware.

## Ordnerlayout

- `reader_stock_v57.bin` — unveränderte Original-Firmware (Quelle für
  `splice.py`/`splice_DWSB20_2TH.py`).
- `reader_modded_89mock_v90.bin` — fertiges Ergebnis: nur der int24-Fix
  (SML TL=0x54), IDA-verifiziert. Diese Datei flashen, wenn beim Zähler
  nur der int24-Fix nötig war (Leistung zeigte „n/a").
- `reader_modded_89mock_v90_DWSB20_2TH.bin` — der DWSB20.2TH-Fix für
  negative Leistung (siehe unten), gebaut per
  `./build.sh DWSB20_2TH && python splice_DWSB20_2TH.py`. Diese Datei
  statt der reinen Variante flashen bei Zählern, bei denen negative/
  Einspeise-Leistung als großer, falscher positiver Wert statt negativ
  ankommt. Auf echter Hardware über längere Zeiträume in beide Richtungen
  sowie über echte Import↔Export-Wechsel live verifiziert (siehe
  „Live-Verifikation" unten).

  **Benennung/Versionierung ist Absicht und für beide Release-Dateien
  identisch**: jede Firmware meldet selbst Softver 89, das RELEASE ist
  aber jeweils als „v90" benannt/getaggt — ein dauerhafter
  „Mock-Version"-Unterschied, kein liegengebliebenes Entwicklungsartefakt.
  Die Web-UI des Gateways liest die zu bewerbende OTA-Zielversion aus dem
  hochgeladenen Dateinamen (`/v(\d+)/`, erster Treffer — hier „v90"), und
  behandelt eine beworbene Version nur dann als No-Op, wenn sie mit dem
  aktuell vom Reader gemeldeten Softver (oder 0) übereinstimmt. Da 90
  niemals mit dem von allen Dateien selbst gemeldeten 89 übereinstimmen
  kann, lässt sich jede Release-Datei beliebig oft erneut
  hochladen/flashen — z.B. um einen Reader zwangsweise wieder auf einen
  bekannt guten Stand zu bringen, oder um einen Reader von einer auf eine
  andere Variante umzustellen — ohne jemals das „schon diese Version,
  übersprungen"-OTA-No-Op einer gleichnummerierten Datei zu treffen. Wer
  mit `splice.py`/`splice_DWSB20_2TH.py` lokal weiter iteriert, sollte
  beide Nummern zueinander passend halten (siehe Kommentar über dem
  jeweiligen Softver-Patch in diesen Skripten) — der 89/„v90"-Unterschied
  gilt nur für die gepinnten Release-Builds.
- `hooks.c`, `entry.S`, `link.ld`, `vendor.h`, `build.sh`, `splice.py`,
  `splice_DWSB20_2TH.py` — gemeinsamer Quelltext
  und Build-Skripte. Der DWSB20.2TH-Fix in `hooks.c` steht hinter
  `#ifdef FIX_NEGATIVE_POWER`, das `./build.sh DWSB20_2TH` setzt;
  `./build.sh` (ohne Argument) baut die reine int24- +
  SML-Int16-Vorzeichenfix-Variante, von der auch `splice.py` ausgeht.
  Alle Varianten lesen dasselbe `hooks.c`/`entry.S` — eine Änderung an
  gemeinsamer Logik (int24-Decoder, Int16-Vorzeichenfix) muss also nur an
  einer Stelle gemacht werden und wirkt sich beim nächsten Build auf alle
  Varianten aus. Das ist der dauerhafte, wiederholbare Release-Prozess —
  bei jeder Änderung an `hooks.c`/`entry.S` `./build.sh && python
  splice.py` UND `./build.sh DWSB20_2TH && python splice_DWSB20_2TH.py`
  ausführen.
- `fix_negative_power.py` — eigenständiges Patch-Skript für einen frühen,
  verworfenen Ansatz (SML-Int8/Int16-Dispatch-Table-Umbiegen). Nur noch
  als historische Notiz vorhanden — bestätigt NICHT der Bug, den der
  DWSB20.2TH tatsächlich auslöst.
- `build/` — NUR kompilierte Zwischendateien: `hooks.*` (reine Variante)
  und `hooks_DWSB20_2TH.*` (DWSB20.2TH-Variante), nebeneinander abgelegt,
  damit sich die Builds nicht gegenseitig überschreiben. `build.sh` legt
  den Ordner bei Bedarf selbst an.

## Bausteine

- `hooks.c`       — die eigentlichen Hook-Funktionen (C, AAPCS, normale
                    Prologe/Epiloge — der Compiler kümmert sich darum).
- `entry.S`        — winzige Glue-Trampoline (Handassembler, wenige Zeilen
                    pro Hook). Diese respektieren die Register-Konvention der
                    jeweiligen Call-Site in der Vendor-Firmware (z.B. "R4 =
                    Ausgabe-Zeiger, R0 = Rückgabewert, POP{R3-R7,PC} am Ende")
                    und rufen dann ganz normal per `bl` in die C-Funktion.
                    Neue Hooks brauchen hier nur ein paar Zeilen.
- `link.ld`        — Linker-Script, das den Code an eine feste Adresse im
                    freien Flash-Bereich hinter dem jeweiligen Firmware-Image
                    legt (aktuell: reader_meter_v57.bin, 44552 B -> Basis
                    0x4000 + 44552 = 0xEE08).
- `build.sh`       — kompiliert+linkt zu `hooks.elf`, extrahiert `hooks.bin`
                    (roher Maschinencode) und `hooks.map`/`hooks.sym`
                    (Symboladressen für splice.py). Mit `DWSB20_2TH` als
                    erstem Argument wird zusätzlich der DWSB20.2TH-Fix
                    (`-DFIX_NEGATIVE_POWER`) in `hooks_DWSB20_2TH.*` gebaut.
- `splice.py` / `splice_DWSB20_2TH.py` — hängen das passende `hooks*.bin` ans
                    Firmware-Image an und patchen die Call-Sites
                    (BL-Encoding programmatisch berechnet) auf die
                    Einsprungpunkte aus `entry.S`. Beide erhöhen auch die
                    eingebetteten Softver-Bytes — **immer auf einen
                    frischen, noch nie verwendeten Wert**, wenn iteriert
                    wird: der Reader-OTA-Pfad überspringt das erneute
                    Flashen still, wenn der eingebettete/beworbene Softver
                    mit einem bereits versuchten übereinstimmt — das sieht
                    dann exakt aus wie „der Fix funktioniert nicht", obwohl
                    tatsächlich nur alter Code lief.

## Ablauf für einen neuen Hook

1. Funktion in `hooks.c` schreiben (normales C, keine libc außer was in
   `vendor.h` deklariert ist). Freestanding `-nostdlib`-Build — es ist
   keine Soft-Division-Laufzeit gelinkt, also `%`/`/` auf
   Nicht-Zweierpotenzen vermeiden.
2. Falls die Call-Site eine Vendor-spezifische Register-Konvention hat
   (nicht AAPCS-Standardaufruf): kleinen Trampolin-Stub in `entry.S`
   ergänzen, der die Register in AAPCS-Argumente (R0,R1,R2,R3) umlegt, `bl`
   in die C-Funktion macht, und danach die von der Call-Site erwartete
   Rückkehr-Sequenz (z.B. `pop {r3-r7,pc}`) ausführt. Bei einem nur für
   DWSB20_2TH gedachten Hook auch die `#ifdef FIX_NEGATIVE_POWER`-Guards in
   `entry.S` nicht vergessen, und dessen Section per `KEEP()` in
   `link.ld` gegen `--gc-sections` absichern.
3. In `splice.py`/`splice_DWSB20_2TH.py` unter `HOOKS = [...]` die
   Call-Site-Adresse + den Namen des entry.S-Symbols eintragen.
4. `./build.sh [DWSB20_2TH] && python splice.py|splice_DWSB20_2TH.py` —
   Ergebnis ist eine neue, gepatchte `.bin`. Vorher das Softver-Patch-Byte
   auf einen frischen Wert setzen (siehe oben).
5. **Immer** per IDA (`idalib_open` + `disasm`) gegenprüfen, bevor
   geflasht wird — das Splicen selbst validiert nur Byte-Encoding, nicht
   Semantik. Direkte Python-Byte-Reads der geflashten `.bin` sind ein
   zuverlässiger Fallback, wenn IDAs Headless-Session mal wieder spinnt.

## SML-Integer16-Vorzeichenfix (`entry_sxth16_fix`)

Gilt **unbedingt für jede Variante** (fest in `hooks.bin` eingebaut, nicht
hinter `FIX_NEGATIVE_POWER` versteckt) — das ist ein allgemeiner Fehler im
SML-Wertedecoder des Herstellers selbst, keine Umgehung für die
Encoding-Eigenheit eines bestimmten Zählers.

`sub_C0A4` (der Low-Level-SML-TL-Byte-Dispatch) hat für Integer16
(`TL=0x53`) einen Fall, der die zwei Wert-Bytes liest und das Ergebnis als
`(byte0<<8)+byte1` berechnet — ein reiner **Zero**-Extend ins breitere
Register der weiteren Pipeline, ohne Vorzeichenerweiterung. Der
benachbarte, fast identische Fall für `TL=0x63` macht denselben
Byte-Read, gefolgt von einem `SXTH R0,R0` (Halbwort vorzeichenerweitern),
bevor das Vorzeichen für das High-Dword berechnet wird — dem
0x53-Fall fehlt genau diese eine Instruktion. Die gesamte
Konvertierungskette danach (`sub_9ED8` → `sub_C41C`/`sub_46D4`/
`sub_4504`/`sub_475C`, Scaler- und Vorzeichen-/Betrags-Behandlung) wurde
durchverfolgt und bestätigt: nichts später korrigiert das Vorzeichen —
`sub_46D4` leitet das Vorzeichen ausschließlich aus dem High-Dword ab, das
`sub_C0A4` übergeben hat, und das ist für diesen Fall immer 0. Ergebnis:
ein roher Integer16-Wert wie `0xFE5F` (-417 dezimal, auf der Leitung
selbst völlig korrekt kodiert) wird als **+65119** gemeldet, nicht als
-417 — der Bit-Musterwert wird als unsigned reinterpretiert, nicht nur
das Vorzeichen umgedreht.

Das kann JEDES SML-Feld betreffen, das als reines Integer16 einen
negativen Wert trägt — am relevantesten die Leistung (16.7.0) eines
Zählers bei Einspeisung, falls dieser Zähler (anders als DWSB20.2TH, das
ein breiteres 24-Bit-Feld mit eigenem, separatem Encoding-Bug nutzt —
siehe unten) Leistung korrekt vorzeichenbehaftet als Integer16 kodiert,
was dieser Reader dann falsch dekodiert.

**Fix**: `entry_sxth16_fix` (`entry.S`) ersetzt die 4 Bytes
`asrs r1,r0,#0x1f ; str r1,[r4,#4]` bei `0xC110` (im 0x53-Fall von
`sub_C0A4`) durch ein `BL` in ein kleines Trampolin, das zuerst das
fehlende `sxth r0,r0` nachholt, dann die Vorzeichenberechnung und den
Store der Originalinstruktionen wiederholt, und über `bx lr` zurückkehrt
(nicht `pop` — das ist kein Ersatz für einen Jump-Table-Slot, sondern ein
Mid-Function-Patch in einer geraden Code-Sequenz, der direkt danach in
den unveränderten Code weiterlaufen muss, der den Low-Dword-Store und den
Sprung in den gemeinsamen Switch-Epilog übernimmt). Kein C-Hook nötig —
der Fix ist reine ALU-Logik, klein genug für direkten Code in `entry.S`.

**Konfidenz**: hoch für die Decode-Analyse (byte-genauer Vergleich des
fehlerhaften und des korrekten Nachbar-Falls, per direktem `get_bytes`
bestätigt, nicht aus dem Gedächtnis zusammengefasst) und für die
Patch-Stellen-Sicherheit (per `xrefs_to` bestätigt, dass nichts in die
Mitte der 4 ersetzten Bytes springt; bestätigt, dass R0/R1 nach diesem
Fall im gemeinsamen Epilog bei `loc_C26C` nicht mehr gelesen werden, also
frei überschreibbar sind; bestätigt, dass LR mitten in der Funktion als
reines Scratch behandelt wird, zwischen Eintritt und den echten
Funktions-Exits nirgends als lebendes Register gelesen wird, also das
eigene `bl` des Trampolins gefahrlos ist, ganz ohne push/pop). **Live
verifiziert** auf echter Hardware: einen Build mit diesem Hook auf den
DWSB20.2TH-Testreader geflasht und bestätigt, dass die normale Dekodierung
über einen vollständigen Negativ↔Positiv-Wechsel hinweg korrekt weiterlief
— das testet nicht den SPEZIFISCHEN Integer16-Negativwert-Pfad
(DWSB20.2TH nutzt für Leistung Integer24, nicht Integer16), bestätigt aber,
dass der Hook selbst `sub_C0A4` bzw. den umgebenden Dispatch bei einem
aktiv Telegramme dekodierenden Reader nicht destabilisiert.

## DWSB20.2TH-Fix für negative Leistung

Byte-genau bestätigt mit einem echten `meter_sniffer`-Mitschnitt von OBIS
1-0:16.7.0 (Leistung): der SML-Encoder des Zählers selbst schafft es
nicht, einen negativen Wert korrekt ins 24-Bit-Feld (SML `Integer24`,
TL=0x54) vorzeichen-zu-erweitern, wenn das korrekte High-Byte exakt
`0xFF` sein müsste — stattdessen kommt `0x00` an. Mitgeschnittenes
Beispiel: die rohen Wert-Bytes `00 EF 4E` decodieren zu +612,62 W, aber
`FF EF 4E` (-42,74 W) war der physikalisch korrekte Wert zu diesem
Zeitpunkt. Beim Scaler dieses Zählers (-2, aus demselben Mitschnitt
bestätigt) ergibt das einen festen Offset von **+655,36 W** (2^16
Roheinheiten) — dieselbe Bug-Klasse wie in einem öffentlichen Bericht zu
genau diesem DWSB20.2TH-Modell dokumentiert
([mikrocontroller.net-Thread](https://www.mikrocontroller.net/topic/564786)),
dessen Community-Workaround exakt dieselben drei Korrektur-Schwellen
(2,56 / 655,36 / 167772,16 W) nutzt — dieser Fix deckt mit 655,36 W nur
die erste davon ab.

Der Bug sitzt auf der Leitung, bevor der Reader überhaupt decodiert —
kein Neu-Parsen kann ein Bit zurückholen, das nie gesendet wurde, und
(live bestätigt) die Größenordnung des empfangenen Werts allein
unterscheidet auf dem tatsächlich aktiven Code-Pfad dieses Readers NICHT
zuverlässig eine echte kleine positive Ablesung von einem korrumpierten
negativen Wert (siehe „Verworfene Versuche" unten). Der Fix gleicht
stattdessen gegen die einzige andere verfügbare Referenz ab: die
kumulativen Import-/Export-Wh-Zähler, die von diesem Bug nicht betroffen
sind (werden als einfache, durch Konstruktion korrekt vorzeichenbehaftete
Deltas gemeldet).

### Aktiver Code-Pfad: `sub_77B4`/`sub_9ED8`

In dieser Firmware gibt es zwei Kandidaten-Pfade zum Parsen von
SML-Telegrammen (`sub_AA7C`s Dispatch, Fälle 1 vs. 2/3). Live-Diagnosen
(unbedingte Sentinel-Werte, die ins übertragene Energie-Payload
geschrieben wurden) bestätigten: `sub_77B4`/`sub_9ED8` (Fall 1) ist der
Pfad, den dieser Reader tatsächlich für die Live-Leistungsmeldung nutzt —
der andere Pfad (`sub_7434`, Fall 2/3) ist für diese Reader/Zähler-
Kombination toter Code.

`hook_power_correct2` hookt `sub_77B4` bei 0x7800, direkt nach
`BL sub_9ED8` — R0 enthält bereits `sub_9ED8`s fertig berechneten,
bereits in Watt skalierten Leistungswert (dieser Pfad hat keinen
separaten Skalierungs-Aufruf wie `sub_7434`s `sub_4700`). Er liest die
eigenen Live-Import-/Export-Zähler des Readers (`unk_20000D68`/
`unk_20000D6C`, dieselbe Struktur, die `sub_77B4` befüllt) und hält eine
kleine persistente Zustandsmaschine im Scratch-RAM
(`0x20001000`-`0x20001010`, empirisch als sicher bestätigt — nicht jede
scheinbar unbenutzte RAM-Adresse ist es tatsächlich, siehe Kommentare in
`hooks.c`):

- **Sticky-Richtungs-Latch mit Alterung**: da die Wh-Zähler bei üblicher
  Leistung nur alle paar Sekunden bis Minuten ticken (deutlich seltener
  als das ~6-s-Leistungsmeldeintervall), verpasst eine strikte
  Prüfung „hat sich der Zähler GENAU IN DIESEM Telegramm
  export-wärts bewegt" fast jede echte negative Ablesung. Stattdessen
  merkt sich der Code, welcher Zähler ZULETZT tickte (Import oder
  Export), und wendet die Korrektur für eine begrenzte Anzahl folgender
  ruhiger Telegramme (`STICKY_MAX_AGE`, aktuell 5) weiter an, bevor er auf
  „unbekannt" (keine Korrektur) zurückfällt.
- Wenn die gelatchte Richtung „Export" ist und der Rohwert im plausiblen
  korrupt-positiven Bereich liegt (`0 <= raw < NEG_FIX_W`, 655 W), wird
  `NEG_FIX_W` abgezogen, um den wahren negativen Wert zurückzugewinnen.
- **Zweiter Korrektur-Bereich (2026-07-11)**: live bestätigt, dass
  Einspeisung oberhalb von ca. 655 W über einen deutlich größeren
  Überlauf statt dessen auftaucht — z.B. zeigte ein echter ~-720-W-
  Moment als +167053..+167098 W an, was für einen Haushaltszähler
  physikalisch unmöglich ist. Dieselbe Bug-Klasse eine Stufe höher: Werte,
  deren Betrag nicht mehr in 16 Bit passt, gehen offenbar über das
  bereits vorhandene Int32-Feld des Zählers (SML TL=0x55, in der
  Stock-Firmware bereits korrekt behandelt — anders als bei Int24 fehlt
  hier kein Fall) statt über Int24 hinaus, und derselbe Bug („neues
  führendes Byte fälschlich 0x00 statt 0xFF") korrumpiert nun das oberste
  Byte eines 32-Bit- statt eines 24-Bit-Werts, was einen Offset von 2^24
  Roheinheiten (167772,16 W bei diesem Zähler-Scaler) statt 2^16
  (655,36 W) ergibt. Gegen das eigene History-Log des Gateways
  reproduziert: die implizierten wahren Werte (dekodierter Wert minus
  167772) passten fast exakt zur gleichzeitigen Tick-Rate des
  Export-Zählers. Anders als der erste Bereich braucht dieser gar kein
  `dir`/Latch-Gating — keine reale Haushaltsablesung kommt unter irgendeiner
  Interpretation auch nur in die Nähe von 167772 W (oder auch nur einem
  Zehntel davon), also wird jeder dekodierte Wert ab `IMPLAUSIBLE_W`
  (50000 W — komfortabel über jeder realen Ablesung und komfortabel unter
  dem niedrigsten Wert, den die Korruption dieses Bereichs erzeugen kann,
  ~83886 W) bedingungslos korrigiert, indem `NEG_FIX_W3` (167772 W)
  abgezogen wird — unabhängig vom aktuellen Zustand des Sticky-Latches,
  was zusätzlich dessen inhärente Verzögerung für diesen Fall umgeht.

`hook_power_correct` (in den mittlerweile als tot bestätigten
`sub_7434`-Pfad bei 0x75EE gehookt) bleibt als harmloses No-Op erhalten —
billige Absicherung falls doch mal eine Telegrammvariante dorthin
gelangt, ohne den Zustand von `hook_power_correct2` zu stören.

### Bekannte, akzeptierte Einschränkung

Ein echter Richtungswechsel ist für diesen Fix erst sichtbar, sobald der
ERSTE Tick des NEUEN Richtungszählers eintrifft — Ticks sind die einzige
an dieser Stelle verfügbare Referenz. Das bedeutet: ein echter Übergang
(z.B. von importdominant zu exportdominant) kann für bis zu
`STICKY_MAX_AGE` Telegramme (Größenordnung eine Minute, je nach
Meldeintervall) ein veraltetes/falsches Vorzeichen zeigen, bevor er
aufholt. Das wurde live beobachtet und ist eine physikalische Grenze, die
sich aus der Abhängigkeit von kumulativen Zählern als einziger
verfügbarer Referenz ergibt — nicht behebbar ohne eine andere Signalquelle,
die an dieser Stelle in der Pipeline nicht zur Verfügung zu stehen scheint.

### Live-Verifikation

Auf echter Hardware verifiziert (OBI-C3-Gateway + gepaarter DWSB20.2TH-
Reader):
- Mehrere, unabhängig beobachtete, minutenlange Phasen korrekter
  positiver Leistung während echter importdominanter Aktivität
  (Import-Zähler tickt, Export bleibt flach), ohne fehlerhafte
  Negativ-Ausschläge.
- Ein echter Export→Import-Übergang, End-to-End im eigenen History-Log
  des Gateways erfasst: ein einzelner Export-Tick löste korrekt das
  Sticky-Latch aus (kleine negative Werte, deren Größenordnung zu
  `NEG_FIX_W` passt, im dokumentierten Korrektur-Bereich — kein
  wilder/falsch-skalierter Wert), das anschließend korrekt alterte und
  wieder auf akkurate positive Werte umschaltete, sobald der Import
  weiter tickte — und blieb für die folgenden 10+ Minuten korrekt.
- Der zweite Korrektur-Bereich wurde direkt nach dem Deployment live
  verifiziert: Einspeisung im Bereich 550-720 W (vorher als +167xxx W
  angezeigt) wurde ab der ersten Ablesung korrekt negativ dargestellt,
  und ein anschließender echter Export→Import-Übergang (Einspeisung bis
  hin zu positiven Import-Werten von ~200 W bis ~1400 W) zeigte in
  keiner Richtung eine Fehlauslösung in einem der beiden Bereiche.
- Per Reader-OTA neu geflasht und bestätigt, dass der eingebettete
  Softver (während der Entwicklung mit 119, dann 120 für den Fix des
  zweiten Bereichs getestet, für den „v90"-benannten Release fest auf 89
  gesetzt — siehe oben) einen Gateway-Neustart und ein Reader-Re-Pairing
  übersteht. Auch der Mock-Version-Mechanismus selbst wurde live
  bestätigt: das erneute Hochladen der „v90"-benannten Datei (bewirbt
  ver=90) auf einen Reader, der bereits auf genau diesem Build läuft (und
  Softver 89 meldet), löste weiterhin ein vollständiges Neuflashen aus,
  statt still als No-Op übersprungen zu werden.

### Verworfene Versuche

Hier festgehalten, damit sie nicht blind nochmal probiert werden:

- **Byte-Scan innerhalb von `hook_decode_int24`** (mehrere Varianten:
  fester -16-Offset, dann ein breiterer 4–24-Byte-Rückwärtsscan, dann eine
  enge Unit-/Scaler-nahe Prüfung), um „dieser Int24-Wert ist OBIS 16.7.0"
  direkt im Low-Level-SML-Decoder zu erkennen, bevor eine feste
  -65536-Korrektur angewendet wird. Jede Variante wurde byte-genau per
  Python-Replay gegen den einen echten `meter_sniffer`-Mitschnitt als
  korrekt bestätigt, aber KEINE feuerte live zuverlässig, auch nachdem
  veraltetes OTA-Caching ausgeschlossen wurde. Das genaue Byte-Layout vor
  dem Wertfeld ist offenbar nicht so konstant, wie der eine Mitschnitt
  nahelegte.
- **Patchen des Endes von `sub_7434`** (0x7604), NACHDEM dessen
  Ausgabe-Struktur bereits befüllt war: die Trend-Logik selbst
  funktionierte (per temporärem Debug-Build bestätigt), aber ein
  Schreibzugriff an dieser Stelle kam nie beim übertragenen
  Energie-Payload an (bestätigt mit einem temporären
  Rohdaten-Debug-Feld auf Gateway-Seite — inzwischen entfernt). Irgendwas
  weiter unten in der Pipeline liest die Struktur über einen Pfad, den
  dieser Hook nicht abfängt — und dieser Pfad erwies sich ohnehin als
  toter Code für diese Reader/Zähler-Kombination (siehe oben).
- **Ein dritter Hook am Callback-Dispatch-Einstieg von `sub_BEE8`**
  (0xBEEC), der den Wert einen Schritt später abfangen sollte. Löste bei
  wiederholten Versuchen zuverlässig einen stillen OTA-Rollback aus
  (komplettes Image übertragen, Reader lief danach still auf der alten
  Version weiter), auch mit defensiv Null-/Bereichs-geprüftem
  Zeigerzugriff — Ursache nicht verstanden, das Risiko war nicht mehr
  wert, nachdem der Fix am `sub_77B4`/`sub_9ED8`-Pfad stattdessen
  funktionierte.
- **Eine bedingungslose Regel „Größenordnung allein beweist negativ"**
  (`raw < 330` impliziert negativ, nach der Theorie, dass eine echte
  kleine positive Ablesung über die kürzere Int16-Kodierung ginge und
  diesen Decoder nie erreichen würde). Live bestätigt: gilt NICHT auf dem
  `sub_9ED8`-Pfad — eine klar importdominante Phase (Import tickt, Export
  statisch) zeigte, dass kleine echte positive Werte (7–27 W) trotzdem
  über denselben Decoder ankamen, und die Regel drehte auch diese ins
  Negative. Zugunsten der reinen Trend-Logik oben verworfen.
- Eine feste Scratch-RAM-Adresse, die laut statischer IDA-Xref-Analyse
  unbenutzt schien (`0x20000D10`, dann `0x20000D20`), hielt live nie
  Zustand über Aufrufe hinweg — selbst mit `volatile` nicht. Irgendetwas
  anderes (vermutlich der Stack) überschreibt sie zwischen
  Hook-Aufrufen. Nach `0x20001000`+ verschoben, empirisch als sicher
  bestätigt.
- Ein einfacher (nicht-`volatile`) Zeiger auf den Scratch-Persistenz-
  Zustand ließ den Compiler das spätere erneute Lesen als aus dem
  früheren Schreiben vorhersagbar behandeln und wegoptimieren/umordnen —
  das Magic-Word überlebte nie bis zum nächsten Aufruf. Behoben durch
  `volatile` auf allen Scratch-Zeigern.

Beide Arten von Lektion zeigen in dieselbe Richtung: lieber am tatsächlich
live genutzten Code-Pfad fixen (per Live-Sentinel bestätigt, nicht nur
statischer Analyse), mit Referenzsignalen, die selbst vom Bug nicht
betroffen sind, statt zu versuchen, einen Wert weiter unten in einer
nicht vollständig kartierten Pipeline abzufangen oder zu überschreiben.

## LoRa-SF9-Option (2026-07-21) — mehr Reichweite, kürzere Akkulaufzeit des Readers, opt-in und ohne Support

Tauscht Übertragungsrate/Sendezeit gegen Reichweite, indem die LoRa-Verbindung vom Standard SF7 auf SF9
umgestellt wird. Beide Seiten müssen übereinstimmen — ein Reader auf SF9 hört kein SF7-Beacon und
umgekehrt, daher ist das immer eine gepaarte Änderung (erst den Reader neu flashen, dann das Gateway
umstellen). Funktioniert und ist live Ende-zu-Ende verifiziert (Taster/Binding, vollständiger
ECDH-Handshake, laufendes Energy-Reporting, Reader-OTA in beide Richtungen) auf echter Hardware
(DWSB20.2TH-Reader `e1c6c5`) — ein früherer Versuch blieb beim Handshake hängen und wurde zurückgerollt;
die Ursache war einer der folgenden vier Patches (#2), keine grundsätzliche Grenze.

**Vier Patches, alle über `splice.py --sf9` / `splice_DWSB20_2TH.py --sf9` zusätzlich zu den normalen
Zähler-Fix-Hooks angewendet** — Release-Builds enthalten jetzt 4 Dateien insgesamt (plain/DWSB20.2TH ×
SF7/SF9), byte-diff-geprüft sauber gegen die jeweilige SF7-Basis (nur die beabsichtigten Bytes ändern sich):

1. **`radio_init_config`-Datarate-Immediates für TX/RX** — `radio_init_config` (beginnt bei `0xB69C`) ruft
   zweimal die Vendor-SX126x-HAL auf (`RadioSetTxConfig`/`RadioSetRxConfig`), jeweils mit dem
   Spreading-Factor als einfachem Ein-Byte-Immediate: `0xB6D8` (`movs r0,#7`→`#9`) für TX, `0xB700`
   (`movs r2,#7`→`#9`) für RX. Gegen jedes andere Stack-/Register-Argument an diesen beiden Aufrufstellen
   gegengeprüft mit der bereits reversten Radio-Tabelle in
   [`../03-reverse-engineering/lora-protocol.md`](../03-reverse-engineering/lora-protocol.md) (Bandbreite
   Index 2, Coderate 1, Preamble 12, TX-Leistung 22 dBm liegen alle genau dort, wo erwartet).
2. **ECDH-Antwort-Timeout verbreitert** (`0x66DA`, 300 ms → 500 ms). Die eigentliche Blockade beim ersten
   Versuch: Die ECDH-Antwortwartezeit des Readers in `ecdh_keyexchange_sm` ist fest auf 300 ms codiert, die
   reale SF9-Rundlaufzeit liegt aber bei ≈360 ms (≈255 ms Gateway-Turnaround + ≈107 ms Sendezeit für die
   68-Byte-Antwort, gegenüber ≈32 ms bei SF7) — der Reader brach also nach 5 Versuchen ab und lief in einer
   Schleife scan→bind→ECDH→ECDH→ECDH, ohne je `key_ready` zu erreichen. Die Verbreiterung auf 500 ms
   (weiterhin ein Ein-Byte-Patch, `adds r3,#45`→`#245`) gibt ≈140 ms Marge über der gemessenen
   SF9-Rundlaufzeit und behob das Livelock.
3. **Ein dritter, unabhängiger SF7-Hardcode** bei `0x6C78` (`movs r1,#7`→`#9`), in einem von IDA nicht
   erkannten TX-Config-Helfer (`0x6C64`-`0x6C92`), erreicht von `sub_D370` aus dem
   Taster-Announce/Bind-Pfad — *nicht* über `radio_init_config`s Aufrufer aus Patch 1 abgedeckt. Ohne diesen
   Patch sendete ein Druck auf den physischen Reader-Taster (erneutes Binding) weiterhin auf SF7, selbst
   nachdem alles andere umgeschaltet war, und wurde damit für ein auf SF9 konfiguriertes Gateway taub.
   Gefunden durch Hand-Disassemblierung vorwärts ab einer bekannt korrekten Funktionsgrenze, nachdem IDA
   diesen Helfer stillschweigend mit seinen Nachbarn verschmolzen hatte.
4. **Gateway-seitige Dual-SF-Behandlung**: Der Bootloader des Readers spricht ausschließlich SF7 (er wurde —
   wie am Anfang dieses Dokuments festgehalten — nie gepatcht, nur die App-Firmware wird verändert), daher
   muss ein auf SF9 konfiguriertes Gateway für die Dauer eines Reader-OTA-Uploads trotzdem auf SF7
   herunterschalten und danach zu SF9 zurückkehren. Siehe `g_liveSF`/`applyLiveSF()`/`gw_ota_arm()`/
   `OTA_SF9_GRACE_MS` in `open_obi_energy_meter/src/main.cpp`. Der Settings-Umschalter selbst
   (`/api/lora/sf`) speichert nur die *gewünschte* Boot-SF im NVS — das Anwenden erfordert immer einen
   Neustart, bewusst niemals ein Live-Hot-Swap, da beide Seiten schon übereinstimmen müssen, bevor eine von
   beiden die Änderung anwendet.

**Gemessener Tradeoff** (derselbe Reader, derselbe Standort, zwei aufeinanderfolgende ~3-Minuten-Fenster):
Der RSSI ist praktisch gleich, wie erwartet — der Spreading Factor ändert nicht die empfangene
Signalstärke, nur die Demodulator-Empfindlichkeitsmarge (SF7 Ø -71,2 dBm vs. SF9 Ø -68,8 dBm, innerhalb
normaler HF-Schwankungen). SNR: SF7 Ø 12,1 dB vs. SF9 Ø 9,2 dB. Bei der tatsächlichen Entfernung dieses
Readers ist keiner der beiden nahe an seiner Rauschgrenze, daher zeigt sich der eigentliche Vorteil von SF9
(≈2,5 dB zusätzliche Empfängerempfindlichkeit pro SF-Stufe) hier nicht als *bessere* Empfangsqualität — er
würde bei einem deutlich weiter entfernten oder stärker verdeckten Reader zum Tragen kommen, was der
eigentliche Grund ist, SF9 statt des SF7-Standards zu wählen, keine allgemeine Empfangs-Verbesserung.

**Akku-Kosten — der eigentliche Tradeoff, nicht die Empfangsqualität**: Die LoRa-Sendezeit (Time-on-Air)
skaliert bei fester Bandbreite grob mit 2^SF. Bei BW500 wächst die Sendezeit der ≈68-Byte-Frames des
Readers von ≈32 ms (SF7) auf ≈107 ms (SF9) — etwa das 3,4-fache an Funk-Einschaltzeit pro Übertragung, für
jeden Announce/Energy-Report/ACK-Austausch, den der Reader macht. Der LoRa-Sender ist einer der größten
Einzelverbraucher im Energiebudget des Readers pro Aufwachzyklus, daher bedeutet das beim gleichen
Report-Intervall eine spürbar kürzere Akkulaufzeit, grob proportional zu diesem Faktor 3,4 (ein längeres
Report-Intervall tauscht das wieder gegen veraltetere Werte ein — ein separater Tradeoff). Das wurde
**nicht** gegen einen echten mehrwöchigen Akku-Entladetest gemessen — den Faktor 3,4 der Sendezeit als
obere Abschätzung für den tatsächlichen Akku-Effekt verstehen, nicht als gemessenen mAh-Wert.

**Nutzung auf eigenes Risiko.** SF9 ist eine nicht unterstützte, zweckentfremdete Konfiguration: Sie
erforderte das Patchen von drei unabhängigen, fest codierten SF7-Annahmen in der Closed-Source-Vendor-
Firmware, die nie für einen anderen Spreading Factor als SF7 ausgelegt war, verifiziert an genau einem
physischen Reader-Modell (DWSB20.2TH) an einem Gateway. Es gibt dafür keinen OBI-/Hersteller-Support, und
die kürzere Akkulaufzeit sowie jede dadurch verursachte Zuverlässigkeitseinbuße liegt vollständig bei
demjenigen, der es aktiviert. Die Settings-Oberfläche des Gateways zeigt beide Punkte direkt neben dem
Umschalter an (siehe `gateway_web.cpp`).

## Warum ein Glue-Stub statt direktem BL in die C-Funktion?

Die Vendor-Firmware benutzt an Patch-Stellen oft KEINE AAPCS-Konvention
(z.B. Rückgabewert nicht in R0 sondern über einen Zeiger in R4, oder ein
gemeinsamer Funktions-Epilog `pop {r3-r7,pc}` statt `bx lr`, oder LR muss
um einen zusätzlichen Aufruf herum gesichert/wiederhergestellt werden, den
die ursprüngliche einzelne Instruktion nie machte). Ein Glue-Stub von
wenigen Zeilen bildet das sauber ab, während der eigentliche Hook (die
Logik) ganz normales, vom Compiler geprüftes C bleibt. Das hält den
handgeschriebenen Assembler-Anteil minimal und lokal auf einen Punkt pro
Hook beschränkt.
