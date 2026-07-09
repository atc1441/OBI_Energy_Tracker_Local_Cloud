# reader_sdk — C-Hook-Framework für die BAT32G135-Reader-Firmware (🇩🇪)

> ➡️ **English version: [../README.md](../README.md)**

Ziel: statt jede Änderung an der (quelloffenen? nein, closed) Vendor-Firmware
per Hand in Thumb-1-Opcodes zu übersetzen, schreiben wir den Hook-Code in C,
kompilieren ihn mit dem echten `arm-none-eabi-gcc` (Cortex-M0+/ARMv6-M,
Thumb-1, kein FPU) und splicen das Ergebnis in die Firmware.

## Ordnerlayout

- `reader_stock_v57.bin` — unveränderte Original-Firmware (Quelle für
  `splice.py`).
- `reader_modded_v87.bin` — fertiges Ergebnis: nur der int24-Fix
  (SML TL=0x54), Softver auf 87 gesetzt, IDA-verifiziert. Das ist die
  einzige Datei, die zum Flashen gebraucht wird.
- `hooks.c`, `entry.S`, `link.ld`, `vendor.h`, `build.sh`, `splice.py` —
  Quelltext und Build-Skripte, liegen direkt hier.
- `build/` — NUR kompilierte Zwischendateien (`hooks.o`, `entry.o`,
  `hooks.elf`, `hooks.map`, `hooks.sym`, `hooks.bin`). `build.sh` legt den
  Ordner bei Bedarf selbst an.

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
                    (Symboladressen für splice.py).
- `splice.py`       — hängt hooks.bin ans Firmware-Image an und patcht die
                    Call-Sites (BL-Encoding wie bisher programmatisch
                    berechnet) auf die Einsprungpunkte aus entry.S.

## Ablauf für einen neuen Hook

1. Funktion in `hooks.c` schreiben (normales C, keine libc außer was in
   `vendor.h` deklariert ist).
2. Falls die Call-Site eine Vendor-spezifische Register-Konvention hat
   (nicht AAPCS-Standardaufruf): kleinen Trampolin-Stub in `entry.S`
   ergänzen, der die Register in AAPCS-Argumente (R0,R1,R2,R3) umlegt, `bl`
   in die C-Funktion macht, und danach die von der Call-Site erwartete
   Rückkehr-Sequenz (z.B. `pop {r3-r7,pc}`) ausführt.
3. In `splice.py` unter `HOOKS = [...]` die Call-Site-Adresse + den Namen
   des entry.S-Symbols eintragen.
4. `./build.sh && python splice.py` — Ergebnis ist eine neue,
   gepatchte `.bin`.
5. **Immer** per IDA (`idalib_open` + `disasm`) gegenprüfen, bevor geflasht
   wird — das Splicen selbst validiert nur Byte-Encoding, nicht Semantik.

## Warum ein Glue-Stub statt direktem BL in die C-Funktion?

Die Vendor-Firmware benutzt an Patch-Stellen oft KEINE AAPCS-Konvention
(z.B. Rückgabewert nicht in R0 sondern über einen Zeiger in R4, oder ein
gemeinsamer Funktions-Epilog `pop {r3-r7,pc}` statt `bx lr`). Ein Glue-Stub
von wenigen Zeilen bildet das sauber ab, während der eigentliche Hook (die
Logik) ganz normales, vom Compiler geprüftes C bleibt. Das hält den
handgeschriebenen Assembler-Anteil minimal und lokal auf einen Punkt pro
Hook beschränkt.
