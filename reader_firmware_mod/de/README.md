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
- `reader_modded_89mock_v90_ISKRA_MT691.bin` — der Fix für „bei ISKRA
  MT-691 wird gar nichts angezeigt" (siehe „ISKRA-MT-691-Fix" unten),
  gebaut per `python splice_ISKRA_MT691.py` (nutzt das `hooks.bin` der
  reinen Variante weiter — kein eigener `build.sh`-Durchlauf nötig).
  Diese Datei statt der reinen Variante flashen, wenn bei einem ISKRA
  MT-691 gar keine Werte ankommen. **NICHT live verifiziert** — komplett
  aus statischer Analyse eines Drittanbieter-SML-Mitschnitts gebaut, es
  stand keine ISKRA-MT691-Hardware oder ein Sniffer zur Verfügung, um den
  Fix in der Praxis zu bestätigen (Details und Konfidenzeinschätzung im
  entsprechenden Abschnitt).

  **Benennung/Versionierung ist Absicht und für alle drei Release-Dateien
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
  mit `splice.py`/`splice_DWSB20_2TH.py`/`splice_ISKRA_MT691.py` lokal
  weiter iteriert, sollte beide Nummern zueinander passend halten (siehe
  Kommentar über dem jeweiligen Softver-Patch in diesen Skripten) — der
  89/„v90"-Unterschied gilt nur für die gepinnten Release-Builds.
- `hooks.c`, `entry.S`, `link.ld`, `vendor.h`, `build.sh`, `splice.py`,
  `splice_DWSB20_2TH.py`, `splice_ISKRA_MT691.py` — gemeinsamer Quelltext
  und Build-Skripte. Der DWSB20.2TH-Fix in `hooks.c` steht hinter
  `#ifdef FIX_NEGATIVE_POWER`, das `./build.sh DWSB20_2TH` setzt;
  `./build.sh` (ohne Argument) baut die reine int24- +
  SML-Int16-Vorzeichenfix-Variante, von der auch `splice.py` UND
  `splice_ISKRA_MT691.py` ausgehen (der ISKRA-Fix ist ein reiner
  zusätzlicher Byte-Patch, kein eigener C-Hook nötig). Alle Varianten
  lesen dasselbe `hooks.c`/`entry.S` — eine Änderung an gemeinsamer Logik
  (int24-Decoder, Int16-Vorzeichenfix) muss also nur an einer Stelle
  gemacht werden und wirkt sich beim nächsten Build auf alle Varianten
  aus. Das ist der dauerhafte, wiederholbare Release-Prozess — bei jeder
  Änderung an `hooks.c`/`entry.S` `./build.sh && python splice.py`,
  `./build.sh DWSB20_2TH && python splice_DWSB20_2TH.py` UND
  `python splice_ISKRA_MT691.py` ausführen.
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
was dieser Reader dann falsch dekodiert. Bestätigt anhand eines echten
Mitschnitts aus einem Drittanbieter-Bericht (ein ISKRA-MT691-Zähler,
siehe „ISKRA-MT-691-Fix" unten), bei dem genau das der Fehlermodus für
eine -417-W-Ablesung war.

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

## ISKRA-MT-691-Fix

**⚠️ NICHT live verifiziert.** Komplett aus statischer Analyse (IDA
Decompile/Disasm) eines echten SML-Mitschnitts gebaut, der von einem
Nutzer weitergegeben wurde, der wiederum den Bericht einer dritten Person
weiterreichte — es stand keine ISKRA-MT691-Hardware, kein Meter-Sniffer
und kein gepaarter Reader zur Verfügung, um den Fix in der Praxis zu
bestätigen. Als Best-Effort behandeln, bis jemand bestätigt, dass er an
einem echten MT691 funktioniert (oder eben nicht).

**Der Bug**: bei einem ISKRA-MT691-Zähler wird GAR NICHTS
dekodiert/angezeigt — kein Vorzeichen-/Größenordnungsproblem wie bei
DWSB20.2TH, komplette Stille. Der gelieferte Mitschnitt wurde von Hand
dekodiert und als vollständig standardkonformes SML ohne eigenen Defekt
in der Zähler-Kodierung bestätigt: ein sauberes `SML_GetList.Res` mit
Einträgen für einen Hersteller-ID-Octet-String ("ISK"), eine
Seriennummer, dann OBIS `1-0:1.8.0` (Import, Unsigned32 + Scaler -1),
`1-0:2.8.0` (Export, gleiches Schema) und `1-0:16.7.0` (Leistung,
Integer16, korrekt vorzeichenbehaftet kodierter Wert -417). Jeder
beteiligte Werttyp (Unsigned32/Integer16/OctetString) wird vom
Stock-SML-Wertetyp-Dispatch bereits unterstützt, und der generische
OBIS-Matcher (`sub_B0C8`) läuft variable-lange Felder auch bei den
ungewöhnlichen führenden Hersteller-ID-/Seriennummer-Einträgen dieses
Zählers korrekt ab — am Telegramm des Zählers liegt es also nicht, anders
als beim DWSB20.2TH-Fall.

**Root Cause**, vollständig per IDA nachverfolgt (Adressen gegen die
tatsächliche Binärdatei verifiziert, nicht aus Namen angenommen):
`sub_9DA0` — der Import-/Export-Multi-Tarif-Cache/Dedup-Helfer
(OBIS 1.8.0–1.8.4 / 2.8.0–2.8.4), aufgerufen von `sub_77B4` — setzt ein
1-Byte-„Unterdrücken"-Flag bei `unk_20000D68+0x10` (Import) /
`+0x11` (Export), wann immer ein im VORHERIGEN Telegramm vorhandenes
Tarifregister im aktuellen fehlt, innerhalb eines 30-Minuten-Fensters.
Das ist ein beabsichtigtes „traue einem gerade verschwundenen Register
nicht"-Dedup-Signal, und `sub_9DA0` setzt es bereits korrekt AUF
WERT-EBENE um: es zwingt den betroffenen Wert beim Auslösen auf das
Sentinel `0x7FFFFFFF`, und die nachgelagerte Meldefunktion `sub_9D34`
macht bei diesem Sentinel bereits sauber nichts. Die Unterdrückung pro
Feld ist also bereits vollständig und korrekt implementiert.

Der eigentliche Bug liegt eine Ebene höher, im äußeren Dispatcher
`sub_AA7C` (`0xAAEE`–`0xAAFD`): er liest EBENFALLS dieselben zwei Flags
und verwirft, wenn EINES gesetzt ist, den KOMPLETTEN dekodierten
Datensatz durch Nullen des Struktur-Zeigers — inklusive des unabhängig
von der unbeteiligten Funktion `sub_9ED8` dekodierten Leistungswerts,
egal welcher von Import/Export NICHT geflaggt ist, und des
„erfolgreich verarbeitet"-Status-Schreibens. Das ist redundanter
Overreach auf einer Logik, die eine Ebene tiefer bereits korrekt
funktionierte, und besonders schädlich für einen Zähler wie den MT691,
der nur EIN Tarifregister meldet (`1-0:1.8.0`, kein `1.8.1`–`1.8.4`): ein
einzelner transienter IR-/Parse-Ausfall dieses einen Registers setzt das
Flag und blendet ALLES aus — Leistung inklusive — für bis zu 30 Minuten,
obwohl das Leistungsfeld im SELBEN Telegramm einwandfrei dekodierte.

**Fix**: den Verwerfungsblock löschen. Reines Entfernen, keine
Ersatzlogik nötig, da die Unterdrückung bereits nachgelagert in
`sub_9D34` korrekt passiert. Per `xrefs_to` bestätigt, dass nichts in die
Mitte dieses 16-Byte-Blocks springt, also sicher als Einheit entfernbar —
aber ein **zweiter Verifikationsdurchlauf hat aufgedeckt, dass reine NOPs
falsch gewesen wären**: `0xAAFE` (direkt nach dem Block) ist keine
Füllung, sondern der Live-Start eines ANDEREN Switch-Falls (erreicht über
ein separates `BEQ` weiter oben in der Funktion, das `sub_7434` für einen
anderen Codepfad aufruft). Bei reinen NOPs wäre man durchgefallen und
hätte bei JEDEM Telegramm unbeabsichtigt den falschen Fall ausgeführt und
das Ergebnisregister überschrieben — ein neuer, schlimmerer Bug als der
zu behebende. Der korrekte (angewendete) Patch ist ein expliziter `B` zum
echten Merge-Punkt `0xAB14`, gefolgt von NOP-Füllung, wobei dieselben 16
Bytes (`0xAAEE`–`0xAAFD`) überschrieben werden. `sub_9DA0`/`sub_B348`/
`sub_C310` bleiben komplett unangetastet, sodass der 30-Minuten-Dedup-/
Cache-Mechanismus für echte Multi-Tarif-Zähler vollständig erhalten
bleibt.

Da die Leistungsablesung dieser Variante (`-417 W`, Integer16) auch nach
diesem Verwerfungs-Fix noch vom separaten Integer16-Vorzeichenfix-Bug
(siehe oben) betroffen wäre, enthält `splice_ISKRA_MT691.py` BEIDE Fixes
— `entry_sxth16_fix` und den `sub_AA7C`-Patch — und nutzt das `hooks.bin`
der reinen Variante weiter (keine `FIX_NEGATIVE_POWER`-gegateten Hooks
nötig, anders als bei der DWSB20.2TH-Variante).

**Konfidenz**: hoch speziell für die Sprung-Offset-Arithmetik (ein
unabhängiger zweiter IDA-Durchlauf hat den Reine-NOP-Bug oben aufgedeckt,
und der finale `B`-Instruktions-Offset wurde unabhängig von Hand
außerhalb von IDA nachgerechnet und stimmte exakt überein, zusätzlich
gegen eine bereits vorhandene `B`-Instruktion an anderer Stelle derselben
Binärdatei gegengeprüft, um die Encoding-Konvention zu bestätigen).
Mittel-hoch für die Root-Cause-Analyse des Verwerfungs-Flags (bei jedem
Schritt durch echtes Disassemblieren/Decompilieren nachverfolgt, nicht
aus Namen abgeleitet) — die verbleibende Unsicherheit ist rein „funktioniert
das tatsächlich am echten Gerät", was sich per Definition ohne einen
echten MT691 nicht bestätigen lässt. Wer einen besitzt und diesen Build
flasht: Rückmeldung (per GitHub-Issue), ob es geholfen hat, wäre
wirklich wertvoll.

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
