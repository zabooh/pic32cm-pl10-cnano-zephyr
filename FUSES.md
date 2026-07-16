# FUSES.md — Die Fuses des PIC32CM PL10, gelesen und interpretiert

Dieses Dokument hält fest, **wo** die Fuses des PIC32CM6408PL10048 liegen, **dass
das Zephyr-Projekt sie nicht anfasst**, **wie** sie ausgelesen wurden und **welche
Werte** auf diesem Board tatsächlich vorgefunden wurden — feldweise gegen das
Datenblatt dekodiert.

> Referenz-Datenblatt-Abschnitte: §5.4 (SIGNATURE), §5.5 (BOOTCFG), §6.4.5
> (Sequence-Number-Check des Boot-ROM).

---

## 1. Fuses sind memory-mapped — aber nicht im Zephyr-Hex

Auf diesem Part liegen die Fuses **nicht** in einem separaten Fuse-Adressraum wie
bei klassischem AVR, sondern **memory-mapped** in einem eigenen NVM-Bereich,
getrennt vom Programm-Flash. Aus dem HAL-Header
(`hal/microchip/.../pic32cm6408pl10048.h`):

| Region | Adresse | Typ |
|---|---|---|
| Programm-FLASH | `0x0c000000` | flash |
| **SIGNATURE (Fuses)** | `0x0d000200` | fuses (read-only, Werk) |
| **BOOTCFG (Fuses)** | `0x0d000400` | fuses (R/W, konfigurierbar) |

Das ist genau das Modell klassischer PIC/PIC32-Configwords: der XC-Compiler legt
`#pragma config`-Werte an ihre memory-mapped Adresse, sie landen im Hex und werden
mitgeflasht.

**Zephyr macht das nicht.** Zwei Gründe:

1. Zephyr baut mit **GCC (`arm-zephyr-eabi`)** — `#pragma config` existiert dort
   nicht (XC-Compiler-Erweiterung).
2. Das Zephyr-Linkerscript platziert Sections nur in die `FLASH`-Region
   (`0x0c00_0000`). An den Fuse-Adressen `0x0d00_02xx`/`0x0d00_04xx` legt der
   Default-Build **nichts** ab.

### Beweis am gebauten Hex

Adressbereich, den `build/zephyr/zephyr.hex` überhaupt abdeckt:

```
lowest  addr in hex: 0x0c000000
highest addr in hex: 0x0c00411c
```

→ reiner Programm-Flash. **Kein einziges Record** bei `0x0d000200` / `0x0d000400`.
`west flash`/pyOCD dieses Projekts schreibt also nur den Code-Flash und **fasst die
Fuses nicht an**. Ihr Zustand ist das, was ein früheres Tool hinterlassen hat
(Werk, MPLAB X / Harmony-Programmierung, Board-Provisionierung).

| | XC / MPLAB X | Zephyr / GCC (dieses Projekt) |
|---|---|---|
| Fuses festlegen | `#pragma config` im Source | gar nicht — kein Mechanismus im Default-Build |
| Landen im Hex? | ja (an Config-Adresse) | nein (nur `0x0c00_0000`-Bereich) |
| Werden geflasht? | ja | nein — behalten Vorwert |

---

## 2. Wie ausgelesen wurde

Die Fuses sind memory-mapped, also über **zwei unabhängige Wege** lesbar — beide
liefern byte-identische Ergebnisse (nur Little-Endian-Bytereihenfolge):

### a) Extern über SWD (pyOCD)

```powershell
# Kern erst wecken (SWD ist im Sleep unerreichbar -> sonst FAULT ACK)
pyocd reset -t pic32cm6408pl10048 -f 100000

# Fuse-Regionen read-only auslesen
pyocd cmd -t pic32cm6408pl10048 -f 2000000 --connect halt -c "read32 0x0d000200 0x40"
pyocd cmd -t pic32cm6408pl10048 -f 2000000 --connect halt -c "read32 0x0d000400 0x80"
```

### b) Vom Kern selbst, über die Board-Konsole (`mem`-Kommando)

Weil die Fuses memory-mapped sind, kann die Firmware sie mit dem vorhandenen
`mem`-Kommando (byteweiser Hex-Dump, `diag.c`) lesen — **ohne Debugger**:

```
pl10:~$ mem 0x0d000400 128
0d000400: 00 00 00 00 ff ff ff ff ff ff ff ff ff ff ff ff  |................|
0d000410: f1 00 f0 ff ff ff ff ff 9d ee fc 70 ff ff ff ff  |...........p....|
0d000420: 3e f8 ff ff ff ff ff ff 60 f0 e5 ff ff ff ff ff  |>.......`.......|
0d000430: ff ff ff ff ...                                   (Rest 0xFF = erased)

pl10:~$ mem 0x0d000200 64
0d000200: 48 00 00 00 ff ff ff ff 53 20 a0 0b ff ff ff ff  |H.......S ......|
0d000210: ff ff fb ff ff ff ff ff fd ff ff ff ff ff ff ff  |................|
0d000220: 42 61 61 85 ff ff ff ff 61 00 25 19 ff ff ff ff  |Baa.....a.%.....|
0d000230: 01 59 01 26 ff ff ff ff 00 00 00 00 ff ff ff ff  |.Y.&............|
```

(`mem` parst die Adresse als Hex und liest byteweise; Fuse-Region über AHB als
Byte-Zugriff problemlos lesbar.)

---

## 3. SIGNATURE @ `0x0d000200` — Werksdaten, read-only (§5.4)

Access = **R**, ab Fertigung gesetzt, nicht änderbar. Enthält u. a. die 128-bit
Unique Serial Number (`SERNUM0..3`).

| Offset | Adresse | Name | Gelesener Wert | Bedeutung |
|---|---|---|---|---|
| 0x00–0x1F | `…200`–`…21F` | *Reserved* | `48000000`, `0ba02053`, `fffbffff`, `fffffffd` | werksinterne Reserved-Daten (Kalibrier/Trim, undokumentiert) |
| 0x20 | `…220` | **SERNUM0** | `0x85616142` | Seriennummer [31:0] |
| 0x28 | `…228` | **SERNUM1** | `0x19250061` | Seriennummer [63:32] |
| 0x30 | `…230` | **SERNUM2** | `0x26015901` | Seriennummer [95:64] |
| 0x38 | `…238` | **SERNUM3** | `0x00000000` | Seriennummer [127:96] |

**128-bit Unique Serial dieses Boards:** `85616142 19250061 26015901 00000000`

Diese Region ist reine Fertigungsidentität — **kein** Hinweis auf Veränderung, sie
ist immer befüllt.

---

## 4. BOOTCFG @ `0x0d000400` — die konfigurierbaren Fuses (R/W, §5.5)

### Überblick der gelesenen Wörter

| Offset | Fuse | Adresse | Gelesener Wert | Werksdefault |
|---|---|---|---|---|
| 0x00 | **SEQNUM** | `0x0d000400` | **`0x00000000`** | **`0xFFFFFFFF`** |
| 0x08 | BOOTPROT | `0x0d000408` | `0xFFFFFFFF` | (BOOTPROT=7 → 0 B Schutz) |
| 0x10 | WDTCFG | `0x0d000410` | `0xFFF000F1` | factory-programmed |
| 0x18 | BODCFG | `0x0d000418` | `0x70FCEE9D` | factory-programmed |
| 0x20 | USERCFG | `0x0d000420` | `0xFFFFF83E` | factory-programmed |
| 0x28 | BOOT_GPIOSEL | `0x0d000428` | `0xFFE5F060` | factory-programmed |

Jedes benannte Fuse-Wort ist belegt, jedes *Reserved*-Wort dazwischen ist
`0xFFFFFFFF` — die Struktur passt exakt zum Datenblatt.

### SEQNUM `0x00000000` (§5.5.1.1, §6.4.5)

Werksdefault = `0xFFFFFFFF`, gelesen = `0x00000000`. NVM-Fuses lassen sich nur
`1→0` programmieren; `0x00000000` = alle 32 Bit bewusst genullt = **gezielter
Schreibvorgang**, kein Rest-/Zufallszustand.

**Wirkung (Boot-ROM):**
- SEQNUM = `0xFFFFFFFF` → keine BOOTCFG-Werte werden angewandt, der Chip bootet auf
  die Peripherie-Reset-Defaults.
- SEQNUM ≠ `0xFFFFFFFF` → das Boot-ROM kopiert **bei jedem Reset** alle BOOTCFG-
  Werte in die Peripherie-Register.

Da hier `0x00000000` steht, ist die folgende Konfiguration **live**.

### BOOTPROT `0xFFFFFFFF`

`BOOTPROT[2:0] = 0b111` = **SIZE_0BYTES** → **kein** Bootloader-Flash-Schutz.

### WDTCFG `0xFFF000F1`

| Feld | Bit | Wert | Bedeutung |
|---|---|---|---|
| ENABLE | 1 | **0** | **Watchdog beim Boot AUS** |
| WEN | 2 | 0 | Window-Mode aus |
| ALWAYSON | 3 | 0 | nicht dauerhaft verriegelt |
| PER / WINDOW / EWOFFSET | – | 0 | irrelevant (WDT aus) |

→ Watchdog wird nicht per Fuse gestartet; Software kontrolliert ihn.

### BODCFG `0x70FCEE9D` (§5.5.1.4)

| Feld | Bit | Wert | Bedeutung |
|---|---|---|---|
| **ENABLE** | 1 | **0** | **Brown-out-Detector beim Boot AUS** |
| LEVEL[1:0] | 17:16 | 00 | BODLEVEL0 = 1.90 V (moot, da aus) |
| ACTCFG | 8 | 0 | Active-Mode: continuous |
| SAMPFREQ | 12 | 0 | 128 Hz |
| RUNSTDBY | 6 | 0 | läuft nicht in Standby |
| STDBYCFG | 5 | 0 | Standby: continuous |
| VLMLVL[1:0] | 25:24 | 00 | **Voltage-Level-Monitor AUS** |
| WRTLOCK | 31 | 0 | BODVDD nicht verriegelt |

→ **BOD per Fuse deaktiviert.** Relevant für Low-Power/Standby (weniger Strom,
aber kein Unterspannungs-Reset). *Notabene:* ab Werk ist BOD üblicherweise **an** —
ENABLE=0 ist auffällig und deutet auf eine bewusste Konfiguration hin.

### USERCFG `0xFFFFF83E` (§5.5.1.5)

| Feld | Bit | Wert | Bedeutung |
|---|---|---|---|
| MVIOMODE | 0 | 0 | MVIO **DUAL** supply mode |
| CRCSEL | 6 | 0 | CRC-16-CCITT |
| CRCBOOT | 7 | 0 | **kein** Boot-CRC-Check |
| SUT[2:0] | 10:8 | 000 | **Start-up-Time = 0 ms** |

### BOOT_GPIOSEL `0xFFE5F060` (§5.5.1.6)

| Feld | Bit | Wert | Bedeutung |
|---|---|---|---|
| **ENABLE** | 7 | **0** | **Boot-External-Notification-Pin AUS** |
| GPIOPINSEL / PORTSEL / POL / ODRAIN / SLEWLIM | – | – | moot (Signal deaktiviert) |

→ Kein Boot-Failure-Notification-Pin aktiv.

---

## 5. Beeinflussen Fuses die Taktung?

Kurz: **Unter den konfigurierbaren Fuses — nein.** Es gibt aber **werksseitige
Kalibrier-Fuses**, die die reale Oszillatorfrequenz bestimmen.

### A) Konfigurierbare BOOTCFG-Fuses → keine Taktwirkung

Die BOOTCFG-Liste (§4) enthält **kein Clock-Select- oder Frequenz-Fuse.** Das
einzige takt-*nahe* Feld ist **USERCFG.SUT** (Start-up Time, hier `0 ms`) — nur die
Anlaufverzögerung nach Reset, **nicht** Quelle oder Frequenz.

Die Taktung nach Reset legt das **Boot-ROM** fest, nicht ein Fuse (§9.3 Clocks
After Reset):

> „After a Device Reset, the Boot ROM configures the OSCHF oscillator to **4 MHz**
> (OSCHF divided by 6) and assigns it as the clock source for GCLK_GEN0 … MCLK."

Frequenz/Quelle werden danach **per Software** gesetzt — `OSCHFCTRL.FRQSEL`
(1/2/3/4/8/12/16/20/24 MHz, §12.6.6) und der GCLK-Baum.

→ **Anders als klassische AVR/PIC** (CKSEL, CKDIV8, Oscillator-Select-Fuses) hat
dieser Part **keine User-Fuse für Taktquelle/-frequenz** — alles Software.

### B) Werksseitige Kalibrier-Fuses → ja, read-only, automatisch geladen

Die *tatsächliche* Frequenz der internen RC-Oszillatoren kommt von
Werks-Kalibrierwerten, die die Hardware bei jedem Reset automatisch in die
Oszillator-Register spiegelt:

| Oszillator | Fuse-Wirkung | Beleg |
|---|---|---|
| **OSCHF** (Hauptoszillator) | factory-calibrated tuning; `OSCHFTUNE.TUNE=0` = keine Zusatzkorrektur über die Werkskalibrierung hinaus | §12.6.7: „nominal value is 0, indicating no correction from the factory-calibrated tuning" |
| **OSC32K** (interner 32 kHz) | Kalibrierwerte werden beim Reset geladen | §13.1: „Calibration values are loaded at reset" |

Diese Werte liegen im **Werks-Kalibrierbereich** — u. a. in den *Reserved*-Werksdaten
der SIGNATURE-Region (`0x0d000200`), die als non-FF gelesen wurden (`48000000`,
`0ba02053`, …). **Read-only, nicht in BOOTCFG, nicht vom User setzbar.**

### Bezug zur Standby-Arbeit

Das erklärt die Standby-Kalibrier-Beobachtung (siehe `STANDBY.md`, `~636 counts/s`
statt 1024 nominal): Wird OSCHF gegatet, läuft der niederfrequente RC ohne seine
übliche Nachführung leicht daneben — die Werkskalibrierung macht ihn im
Normalbetrieb nominal genau, deckt aber diesen entkoppelten Standby-Fall nicht ab.

## 6. Gesamtbild & Fazit

Dieses Board läuft mit einer **aktiven, gefusten Boot-Konfiguration**, die bei
jedem Reset greift:

- **Watchdog aus**, **Brown-out aus**, **VLM aus**, **kein Boot-Flash-Schutz**,
  **kein Boot-CRC**, **0 ms Startup**, MVIO Dual-Supply.

Zur Frage „Werkszustand oder von einem Tool verändert":

- **SEQNUM = `0x00000000` ist der eindeutige Beweis, dass programmiert wurde**
  (Werk = `0xFFFFFFFF`). Etwas hat die BOOTCFG vor diesem Zephyr-Projekt
  beschrieben.
- Die übrigen Wörter (WDTCFG/BODCFG/USERCFG/BOOT_GPIOSEL) tragen spezifische Werte
  und sind jetzt live. Ob sie exakt die Microchip-Werksprogrammierung sind oder
  mitverändert wurden, ist aus den Bits allein nicht endgültig entscheidbar (dazu
  bräuchte man die dokumentierte Werks-Hex je Fuse). **BOD aus** ist aber für
  reinen Werkszustand untypisch und passt zu einer früheren
  MPLAB-X-/Harmony-Konfiguration dieses Boards.
- **Zephyr / `west flash` war es nicht** — am Hex-Adressbereich bewiesen
  (nur `0x0c00_0000`, siehe §1).

### Falls die Fuses aus dem Zephyr-Build heraus gesetzt werden sollen

Es gibt keinen `#pragma`-Weg, aber die GCC-Entsprechung:

1. **Eigene Linker-Section** an `0x0d000400` (Fragment via `zephyr_linker_sources()`,
   wie `cmd_sections.ld`) + `const`-Struct mit `__attribute__((section(...)))` mit
   den Fuse-Bytes → landet im `zephyr.hex`. **Vollständiges Rezept in §7.**
2. **Extern setzen** über MPLAB X / Studio „Configuration Bits" (üblicher, robuster).
3. **Zur Laufzeit** über NVMCTRL beschreiben (Direct-Register, wie `standby.c`).

---

## 7. Fuses aus dem Zephyr-Build ins Hex legen (Produktions-Absicherung)

> **Status: dokumentiert, noch nicht umgesetzt.** Später bei Bedarf einbauen.

**Motivation (der „böse Falle"):** Steht die BOOTCFG-Region **nicht** im gelieferten
Hex, kann ein Programmier-Tool beim Fremdfertiger **eigene Defaults** auf die Fuses
anwenden (oder sie in einem undefinierten Zustand lassen). Legt man die gewünschten
Fuse-Werte explizit ins Hex, programmiert ein vollwertiger Produktions-Programmer
genau diese Werte — kein Interpretationsspielraum.

**Prinzip:** Ein `const`-Objekt mit den BOOTCFG-Bytes wird per eigener Linker-Section
auf die **absolute Adresse `0x0d000400`** gezwungen; `objcopy` emittiert es dann
automatisch als Records ins `zephyr.hex`. Das ist die GCC-Entsprechung zu XCs
`#pragma config`.

### Baustein 1 — Fuse-Werte als C-Objekt (`app/src/bootcfg_fuses.c`)

```c
#include <stdint.h>

struct pl10_bootcfg {
    uint32_t seqnum;        /* 0x00 */
    uint32_t _rsv0;         /* 0x04 */
    uint32_t bootprot;      /* 0x08 */
    uint32_t _rsv1;         /* 0x0c */
    uint32_t wdtcfg;        /* 0x10 */
    uint32_t _rsv2;         /* 0x14 */
    uint32_t bodcfg;        /* 0x18 */
    uint32_t _rsv3;         /* 0x1c */
    uint32_t usercfg;       /* 0x20 */
    uint32_t _rsv4;         /* 0x24 */
    uint32_t boot_gpiosel;  /* 0x28 */
};

const struct pl10_bootcfg __attribute__((section(".bootcfg_fuses"), used))
bootcfg_fuses = {
    .seqnum       = 0xFFFFFFFF,   /* <-- SIEHE FALLE 1, bewusst waehlen! */
    ._rsv0        = 0xFFFFFFFF,
    .bootprot     = 0xFFFFFFFF,   /* 0 B Boot-Schutz */
    ._rsv1        = 0xFFFFFFFF,
    .wdtcfg       = 0xFFF000F1,   /* WDT aus */
    ._rsv2        = 0xFFFFFFFF,
    .bodcfg       = 0x70FCEE9D,   /* BOD aus */
    ._rsv3        = 0xFFFFFFFF,
    .usercfg      = 0xFFFFF83E,   /* SUT 0 ms, MVIO dual */
    ._rsv4        = 0xFFFFFFFF,
    .boot_gpiosel = 0xFFE5F060,   /* Boot-Notification aus */
};
```

`used` verhindert Wegoptimieren des nie referenzierten Objekts.

### Baustein 2 — Linker-Fragment auf absolute Adresse (`app/bootcfg_sections.ld`)

```
.bootcfg_fuses 0x0d000400 :
{
    KEEP(*(.bootcfg_fuses))
}
```

Explizite Adresse (kein `>FLASH`) → landet nicht im Programm-Flash. Zephyrs eigene
Sections sind region-anchored (`>FLASH`) und laufen unbeeinflusst weiter; der globale
Location-Counter ist hier kein Problem.

### Baustein 3 — CMake-Verdrahtung (`app/CMakeLists.txt`)

```cmake
target_sources(app PRIVATE ... src/bootcfg_fuses.c)   # an bestehende Zeile anhaengen
zephyr_linker_sources(SECTIONS bootcfg_sections.ld)   # zusaetzlich zur cmd_sections.ld-Zeile
```

### Verifikation

Nach Pristine-Build muss der Hex-Adressbereich die Fuse-Adresse enthalten:

```
lowest  addr in hex: 0x0c000000
highest addr in hex: 0x0d00042b   <-- vorher 0x0c00411c
```

plus Record-Block bei `0x0d000400`. Gegenprobe per `mem 0x0d000400 48` bzw.
`pyocd cmd ... read32 0x0d000400`.

### Die drei Fallen

**Falle 1 — SEQNUM entscheidet über *alles* (§5.5, §6.4.5).** Das Boot-ROM wertet
SEQNUM aus:
- `SEQNUM = 0xFFFFFFFF` → **keine** BOOTCFG-Werte werden angewandt (Peripherie-Reset-
  Defaults). Die anderen Wörter im Hex sind dann *wirkungslos*.
- `SEQNUM ≠ 0xFFFFFFFF` → **alle** BOOTCFG-Wörter werden bei jedem Reset angewandt.
  Datenblatt: *„all BOOTCFG fuses must be written to desired values — the factory
  values have no practical meaning."*

→ Entweder **Werksverhalten festnageln** (`seqnum = 0xFFFFFFFF`, Rest egal) **oder**
**eigene Config** (`seqnum` bewusst auf Nicht-FF **und jedes** Feld korrekt füllen —
Halb-Befüllen ist der Bug).

**Falle 2 — Fuses sind NVM: nur `1→0`, Erase nötig.** `0→1` braucht vorher einen
Fuse-Page-Erase; zusätzlich ist die BOOTCFG-Section **vom normalen Chip-Erase
unberührt**. Der Produktions-Programmer muss explizit auf „Config/Fuse-Page erase +
program" konfiguriert sein — das gehört in die Programmier-Spezifikation an den
Fremdfertiger.

**Falle 3 — nicht jedes Flash-Tool programmiert `0x0d000400`.** `west flash`/pyOCD
schreibt diese Region evtl. **nicht** (Flash-Algo zielt auf Programm-Flash; Records
bei `0x0d000400` werden ggf. mit Warnung übersprungen). Der Hex-Eintrag nützt
trotzdem: ein vollwertiger Produktions-Programmer (MPLAB IPE / Gang), der das
komplette Hex ehrt, schreibt die Fuses. **Einmal mit dem echten Produktionswerkzeug
verifizieren, nicht nur mit pyOCD.**

**Nebeneffekt — `zephyr.bin` wird riesig.** `.bin` ist ein flaches Image von
niedrigster bis höchster LMA → bläht sich um die ~16 MB Lücke ab `0x0c000000` auf.
**Für Produktion `zephyr.hex` verwenden** (sparse), nicht `zephyr.bin`.

### Empfehlung

1. Bewusst entscheiden: Werksverhalten (`SEQNUM=0xFFFFFFFF`) oder eigene Config
   (SEQNUM≠FF + alle Felder).
2. Bausteine 1–3 einbauen, Hex verifizieren.
3. Dem Fremdfertiger **schriftlich** mitgeben: „Config/BOOTCFG-Page erase + program
   aus dem gelieferten Hex, Verify über den kompletten Adressraum inkl. `0x0d0004xx`."
4. Alternative: ein separates `fuses.hex` pflegen und beide Artefakte liefern —
   sauberere Trennung, aber zwei Dateien.
