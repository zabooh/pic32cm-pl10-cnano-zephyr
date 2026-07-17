# mini/ — bare-metal current-measurement firmwares

Tiny **no-Zephyr** firmwares for the PIC32CM PL10, used to characterise / verify
the board's current draw independent of the whole Zephyr stack. They come out of
reset, touch almost nothing, and let you see the MCU's real controllable range —
handy to reproduce the measurements or to keep hacking on a minimal low-power
baseline.

## The two firmwares

| File | What it does |
|---|---|
| **`while1.s`** | Absolute minimum: reset → `while(1)` busy loop. Zero init. The boot ROM leaves OSCHF running; the CPU just spins. Raw CPU+board base. |
| **`step.c`** (+ `step_vectors.s`) | A 4-step sequence, each busy phase timed ~3 s by the RTC on OSC32K: **OSCHF busy → OSCHF off (CPU to 32 kHz) → OSC32K busy → Standby**. Shows the MCU's whole range as clean current steps. |

## Measured results (POWER-Z KM003C, USB rail)

| State | Current |
|---|---|
| `while1` (CPU busy on OSCHF ~4 MHz) | ~16.2 mA |
| `step` Phase 1 — CPU busy on OSCHF | 16.20 mA |
| `step` Phase 2 — CPU busy on OSC32K (32 kHz) | 15.33 mA |
| `step` Standby (OSCHF off, WFI) | 15.33 mA |

**Interpretation:** the MCU's entire software-controllable range is only
**~0.9 mA** (busy@4 MHz → standby), of which OSCHF is ~0.44 mA. Everything below
that — the **~15.3 mA floor — is the on-board nEDBG debugger chip + power LED +
regulators**, not the target MCU. In Standby the MCU itself draws ~µA (datasheet
~2 µA), which is invisible from the USB rail because the nEDBG floor sits on top
of it. To see the MCU's real microamps you must meter the isolated target rail
(cut **J201**, User Guide §5.5) — these firmwares are ideal for that: flash
`step.hex`, cut J201, and the steps will show the true MCU current.

**OSCHF gotcha:** the SoC boots with `OSCHFCTRL.ONDEMAND=0` (free-running), so
OSCHF never stops on its own. It becomes gateable only after setting `ONDEMAND=1`
**and** removing its requester (moving the CPU/GCLK0 onto OSC32K). `step.c` does
exactly this — see its comments.

## Build

Needs the Zephyr SDK ARM toolchain (`arm-zephyr-eabi-*`), same one the main build
uses. From Git Bash:

```bash
./build.sh          # -> while1.hex, step.hex
```

If your SDK is not at `$HOME/zephyr-sdk-1.0.1`, set `ZEPHYR_SDK` or edit `SDK_VER`
at the top of `build.sh`. Headers come from the repo's pinned `modules/hal/...`.

> **Note:** `build.sh` links with `ld` **directly**, not through `gcc`. The gcc
> link driver drops the C `.text` at an orphan `0x10000000` instead of into FLASH;
> `ld` + the provided linker scripts place everything at `0x0c000000` correctly.

## Flash

```bash
pyocd reset -t pic32cm6408pl10048 -f 100000        # wake/clear the DAP first
pyocd flash -t pic32cm6408pl10048 -f 100000 step.hex
pyocd reset -t pic32cm6408pl10048 -f 100000        # run it (step runs once per reset, then sits in Standby)
```

`step` runs its 4-phase sequence once on every reset, then stays in Standby until
the next reset. `while1` just spins forever.

## Restore the normal Zephyr firmware

These overwrite only the first flash page, so:

```bash
cd ../build && west flash      # re-flashes the Zephyr app; the rest was untouched
```

## Notes / next steps

- Memory map (from the SoC devicetree): FLASH `0x0c000000` (64 KB), SRAM
  `0x20000000` (8 KB → initial SP `0x20002000`).
- No `.data`/`.bss` init is done (the code uses no initialised globals), so no C
  startup is needed — that is why `-nostartfiles` is enough.
- Good starting point for: a real resume-in-place low-power path, EIC/pin wake
  experiments, or a clean J201-rail µA verification.
