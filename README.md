# PIC32CM PL10 Curiosity Nano — Lean Zephyr Blinky

Minimal, from-scratch [Zephyr RTOS](https://www.zephyrproject.org/) workspace for the
**Microchip PIC32CM PL10 Curiosity Nano** board, plus a pinned, non-interactive script
that reproduces the entire installation — RTOS clone, HAL modules, Python venv,
toolchain, build, and flash — on another Windows machine.

## Start here

**Fresh Windows machine, no Zephyr yet?** Clone this repo, make sure the five host tools
are present (a helper installs any that are missing), then run the one reproduction
script. It clones a PL10-tailored Zephyr (~1.8 GB), sets up the toolchain, builds the
app, and — if a PL10 Curiosity Nano is plugged in — flashes it, all in one go
(~5–20 min):

```powershell
git clone https://github.com/zabooh/pic32cm-pl10-cnano-zephyr.git C:\zw
cd C:\zw
powershell -ExecutionPolicy Bypass -File install-prerequisites.ps1   # installs any missing host tool; then open a new shell
powershell -ExecutionPolicy Bypass -File reproduce-install.ps1
```

`install-prerequisites.ps1` leaves already-installed tools untouched and only pulls the
pinned version of whatever is missing (Python, Git, Ninja, CMake, 7-Zip, via winget) —
skip it if you already have all five. **Open a new terminal between the two scripts** so
freshly installed tools land on `PATH`. Details and exactly what gets version-pinned:
[Reproducing this setup elsewhere](#reproducing-this-setup-elsewhere). Keep the target
path short (Windows' ~260-char limit; see [Quick start](#quick-start)).

**Already ran this once and just want to rebuild / re-flash?** → [Quick start](#quick-start).

## Contents

- [Start here](#start-here)
- [Executive summary](#executive-summary)
- [Key takeaways](#key-takeaways)
- [Quick start](#quick-start)
- [Project structure](#project-structure)
  - [Hardware](#hardware)
  - [Architecture](#architecture)
- [Working in VS Code](#working-in-vs-code)
  - [Recommended workflow: source-line debugging](#recommended-workflow-source-line-debugging)
- [Reproducing this setup elsewhere](#reproducing-this-setup-elsewhere)
- [Design decisions](#design-decisions)
- [Updating the Zephyr version](#updating-the-zephyr-version-moving-the-pin)
- [Peripheral support status](#peripheral-support-status)
- [Bridging a peripheral before Zephyr supports it](#bridging-a-peripheral-before-zephyr-supports-it)
- [Interrupts and the system tick](#interrupts-and-the-system-tick)
- [The driver model behind a pin toggle](#the-driver-model-behind-a-pin-toggle)
- [Idle, WFI, and CPU load](#idle-wfi-and-cpu-load)
- [Memory usage](#memory-usage)
- [Known issues](#known-issues)
- [What is `RUNBOOK.md`?](#what-is-runbookmd)
- [PIC32CM PL10 device series](#pic32cm-pl10-device-series)
- [Porting to another PL10 device](#porting-to-another-pl10-device)

## Executive summary

This is a small interactive firmware — LED control, ADC sampling, and a reset over a
serial command line — built to answer one question: **can Zephyr RTOS run comfortably on
an 8 KB-RAM microcontroller**, or does it need a beefier part to be worth using?

- Interactive serial commands: `led on/off/toggle/blink <ms>`, `adc read`,
  `adc stream start/stop`, `button` (read the SW0 user button), `reset`,
  `threads` (live per-thread stack usage), `mem <addr>` (raw hex dump), `help` — with a
  hand-rolled line editor featuring bash-style Up/Down command history, and a boot banner
  stamped with the build time
- Genuinely multi-threaded: six threads (LED blink, ADC stream, command console, button
  watcher, plus the kernel's main/idle) — a real RTOS app, not a superloop; a `threads`
  command shows each one's live stack high-water mark
- Fully reproducible setup: one script rebuilds the whole toolchain (Zephyr, HAL
  modules, SDK, Python deps) from scratch on any Windows machine, pinned to exact
  versions
- Lean by construction: only the Zephyr modules this board actually needs are cloned
  (not the dozens of unrelated HALs/subsystems a default `west update` would pull in),
  keeping the whole installation **under 2 GB** instead of several GB
- VS Code integration: build/flash tasks, source-line debugging, IntelliSense
- Only **64% RAM / 31% flash** used of this board's 8 KB RAM / 60 KB flash — still
  ~36% of RAM free with all of the above running (thread introspection included)

Where to go from here:
- Just want to build and flash it? → [Quick start](#quick-start)
- Want to reproduce this install elsewhere? → [Reproducing this setup elsewhere](#reproducing-this-setup-elsewhere)
- Want to understand *why* a decision was made? → [`RUNBOOK.md`](RUNBOOK.md)

## Key takeaways

- Zephyr RTOS runs comfortably on the PIC32CM PL10's 8 KB RAM — contrary to the common
  assumption that it needs a bigger part.
- A real multi-threaded interactive app (LED blink + ADC streaming + a command console
  with history + a button watcher + thread introspection, six threads total) still leaves
  **~36% of RAM free** — 5,232 B used of 8 KB. A bare blinky started far lower (2,744 B / 33.50%; see
  [Memory usage](#memory-usage)).
- The entire toolchain — Zephyr revision, HAL modules, SDK, Python packages — is
  version-pinned and reproducible with one script on a fresh Windows machine.
- Source-line debugging works reliably in VS Code once a couple of Windows-specific
  Cortex-Debug/pyOCD quirks are worked around (documented once, not re-discovered every
  time — see [Known issues](#known-issues)).
- Staying lean is a choice, not luck: no Zephyr shell subsystem (a hand-rolled parser
  instead), only the four HAL/lib modules this board needs, and trimmed kernel stacks.

## Quick start

> `C:\zw` throughout this README (and `RUNBOOK.md`) is just an example path, not a
> requirement — use whatever directory you like. Keep it **short**, though: Zephyr's
> build tree nests deeply, and a long workspace path can hit Windows' ~260-character
> path limit (see "Path too long" in `RUNBOOK.md` → Troubleshooting).

On a machine already set up (venv, SDK, and workspace in place):

```powershell
& C:\zw\.venv\Scripts\Activate.ps1
Set-Location C:\zw\zephyr
$env:CMAKE_GENERATOR = "Ninja"
west build -p always -b pic32cm_pl10_cnano -d C:\zw\build C:\zw\app

Set-Location C:\zw\build
west flash
```

**In `cmd.exe` instead of PowerShell**, use `activate.bat` and `set` instead of
`Activate.ps1`/`$env:...`:

```cmd
C:\zw\.venv\Scripts\activate.bat
set CMAKE_GENERATOR=Ninja
cd C:\zw\zephyr
west build -p always -b pic32cm_pl10_cnano -d C:\zw\build C:\zw\app

cd C:\zw\build
west flash
```

> ⚠️ Activate the venv first — calling `west.exe`/`pyocd.exe` by full path without
> activating works for `build` but not for `flash`. See [Known issues](#known-issues).

**Straight from the clone root** — in `cmd.exe`, after `cd`-ing into the cloned directory,
build and flash with relative paths only (nothing hardcoded, works on any machine):

```cmd
.venv\Scripts\activate.bat
set CMAKE_GENERATOR=Ninja
cd zephyr
west build -p always -b pic32cm_pl10_cnano -d ..\build ..\app
cd ..\build
west flash
```

> If `west flash` stops with `SWD/JTAG communication failure (FAULT ACK)`, the core is
> asleep / the debug port is in a leftover state — or a GDB server still holds the probe
> (stop any debug session first; only one pyOCD can hold the probe). Reset once and retry,
> from the `build` dir:
>
> ```cmd
> pyocd reset -t pic32cm6408pl10048 -f 100000
> west flash
> ```
>
> The VS Code *Zephyr: Flash* task does this reset automatically.

Connect a serial terminal (PuTTY, TeraTerm, ...) to the board's virtual COM port at
**115200 baud**. A boot banner with the build timestamp prints, then the prompt
`pl10:~$ `. Type a command and press Enter; **Up/Down arrow** recalls the last five
commands. Try `help` for the full list:

```
pl10:~$ led on
pl10:~$ led blink 100
pl10:~$ adc read
adc: 3676 (2.962 V)
pl10:~$ adc stream start
adc: streaming every 500 ms
adc: 3688 (2.972 V)
...
pl10:~$ adc stream stop
pl10:~$ button
button: released
pl10:~$ button: pressed      <-- printed on its own when you press SW0
button: released
pl10:~$ reset
```

(On the bare board the ADC input pin floats, so `adc read` returns noisy values near the
supply rail — that's expected, it confirms the ADC is actually converting.)

Don't have a set-up machine yet? See [Reproducing this setup elsewhere](#reproducing-this-setup-elsewhere).

## Project structure

| Path | Purpose |
|---|---|
| `app/` | The application: `CMakeLists.txt`, `prj.conf`, `app.overlay` (SW0 user-button devicetree node), `cmd_sections.ld` (command-registry linker section), and `src/` split one module per domain — `main.c` (startup wiring), `led_ctrl.c` (LED + blink thread + `led` command), `pl10_adc.c` (ADC driver + stream thread + `adc` command), `button.c` (SW0 read + press-notifier thread + `button` command), `cmd_parser.c` (console infrastructure: line editor, history, registry dispatch), `cmd.h` (command-registry interface), `diag.c` (`reset`/`threads`/`mem` commands), `fault.c` (fatal-error handler), `app_threads.h` (central thread stack/priority budgets) |
| `zephyr/` | Zephyr RTOS source (shallow clone, pinned revision) |
| `modules/` | Only the HAL/library modules actually needed: `hal_microchip`, `cmsis`, `cmsis_6`, `picolibc` |
| `build/` | CMake/Ninja build output; `build/zephyr/zephyr.hex` is the flashable artifact |
| `.venv/` | Python virtual environment (west, pyOCD, build dependencies) |
| `install-prerequisites.ps1` | Checks for the host tools (Python, Git, Ninja, CMake, 7-Zip) and installs the pinned version of any that are missing (via winget); leaves existing installs untouched |
| `reproduce-install.ps1` | Pinned, non-interactive script that recreates this whole setup from scratch |
| `requirements-lock.txt` | Exact pinned versions of every Python package in `.venv` |
| `.vscode/` | Build/flash tasks, debug configs, and IntelliSense settings for working in VS Code (see below) |
| `RUNBOOK.md` | Step-by-step build log with rationale, troubleshooting table, and lessons learned |

### Hardware

- Board: **PIC32CM PL10 Curiosity Nano** (Zephyr board name `pic32cm_pl10_cnano`, SoC `pic32cm6408pl10048`, Arm Cortex-M0+)
- On-board debugger: Microchip **nEDBG** (CMSIS-DAP over USB), also exposes a virtual COM port for the serial console
- No external debug probe needed — just a USB data cable

### Architecture

**In plain words:** the app in `app/` is a small firmware that blinks an LED and listens
for typed commands over the serial port. You type a command in a terminal and the board
acts on it — control the LED (`led`), read the analog input (`adc`), read the user button
(`button`), inspect the running threads or raw memory (`threads`, `mem`), or reboot
(`reset`); **Up/Down** recalls previous commands. Pressing the on-board button also prints a
message on its own. It's deliberately a real multi-threaded RTOS app — the LED blink, the ADC
stream, the button watcher, and the console run concurrently, not in one big loop — as a
demonstration that Zephyr fits
comfortably on this 8 KB-RAM chip. The organizing idea is **one small module per job**,
wired together through a tiny flash-resident command registry instead of Zephyr's
RAM-heavy shell.

Concretely: the app is split one module per functional domain. No shell subsystem — the
command console is hand-rolled on `console_getchar()`. `cmd_parser.c` is **feature-agnostic
infrastructure**: it tokenizes a line and dispatches by looking the command up in a
flash-resident **command registry**. Each feature module *self-registers* its command with
one `CMD_REGISTER()` macro (`cmd.h`), so the parser has no dependency on `led_ctrl` /
`pl10_adc` / `diag` and adding a command touches only its owning module.

```
  app/src/  (6 threads: main + idle + blink_tid + adc_stream_tid + cmd_tid + button_tid)

  +-------------------------------+
  | cmd_parser.c   (cmd_tid)      |  feature-agnostic console:
  | console_getchar, line editor, |  tokenize a line, look argv[0]
  | history, registry dispatch    |  up in the registry, dispatch;
  |                               |  built-in "help"
  +---------------+---------------+
                  |  STRUCT_SECTION_FOREACH(cmd, ...)
                  v
  +-------------------------------------------+  "cmd" iterable ROM section
  |          command registry  (flash)        |  (cmd.h / CMD_REGISTER, ~0 B RAM)
  +---^---------^---------^---------^----------+
      |         |         |         |   each module self-registers its command(s)
  +---+----+ +--+-----+ +-+------+ +-+------+
  |led_ctrl| |pl10_adc| |button.c| | diag.c |
  |blink_ti| |adc_strm| |button_t| | reset  |
  | GPIO   | | ADC0   | | SW0    | | threads|
  | "led"  | | "adc"  | |"button"| | mem    |
  +---+----+ +--+-----+ +-+------+ +-+------+
      |         |         |          |    (fault.c overrides the fatal handler)
      v         v         v          v
  +-----------------------------------------------------------+
  |  Zephyr RTOS kernel   (GPIO, UART, clock drivers)         |
  +----------------------------+------------------------------+
                               v
  +-----------------------------------------------------------+
  |  HAL / library modules: hal_microchip, cmsis, cmsis_6,    |
  |  picolibc   (ADC0 register defs come from hal_microchip)  |
  +----------------------------+------------------------------+
                               v
  +-----------------------------------------------------------+
  |  PIC32CM PL10  (Arm Cortex-M0+)                            |
  +-----------------------------------------------------------+

  main.c: startup wiring only (led_ctrl_init + default blink).
```

**Footprint on the PL10** (8 KB RAM / 60 KB flash): about **5.1 KB RAM (~64 %)** and
**19 KB flash (~31 %)** — RAM is the tight resource, flash is roomy (two-thirds free).
Most of the RAM is the four app threads (each: its stack — 640 B console, 384 B button,
352 B blink, 352 B ADC — plus a ~120 B control block); dropping the Zephyr shell in favor of
the hand-rolled parser is a big part of why a genuinely multi-threaded app still leaves ~36 %
of RAM free. Full per-symbol breakdown: [Memory usage](#memory-usage).

## Working in VS Code

Open the workspace root itself in VS Code — the cloned directory that contains `app/`,
`zephyr/`, and `.vscode/` (not a subfolder). The checked-in `.vscode/` configs make
**build, flash, and debug** work with no per-machine editing (all paths are relative or
resolved via `${workspaceFolder}` / `${env:USERPROFILE}`). `.vscode/extensions.json` will
prompt you to install the two extensions this setup relies on:

- **C/C++** (`ms-vscode.cpptools`) — IntelliSense, backed by `build/compile_commands.json`
- **Cortex-Debug** (`marus25.cortex-debug`) — GDB/pyOCD debugging UI

**Build** — `Ctrl+Shift+B`, or Terminal → Run Task:
- *Zephyr: Build (pristine)* (default) — full `-p always` rebuild, also regenerates `compile_commands.json`
- *Zephyr: Build (incremental)*
- *Zephyr: Menuconfig*
- *Zephyr: Clean build dir*

**Flash** — Terminal → Run Task → *Zephyr: Flash*. Runs a pristine build, then resets the
board and flashes in one step — always shipping a fresh binary. The reset first clears the
sleeping-core / leftover-debug DAP state that otherwise makes flashing fail with
`SWD/JTAG communication failure (FAULT ACK)` (see [Known issues](#known-issues)). Picks up
the board's SWD frequency from `board.cmake` automatically. **Stop any debug session and
the *Start GDB Server (pyOCD)* task before flashing** — only one pyOCD instance can hold
the USB probe at a time, and a lingering GDB server is itself a common cause of the
`FAULT ACK`.

**Debug** — Run and Debug panel (`Ctrl+Shift+D`) offers three configs; the reliable one
on Windows is *Zephyr: Debug (attach to running GDB server) - start server task first!* —
see the workflow below for why and how. All tasks call `west`/`pyocd` directly from
`.venv/Scripts/` (no need to activate the venv first) and use the pinned `pyocd==0.43.0`
explicitly, so Cortex-Debug can't accidentally pick up a different (broken) pyOCD from
`PATH`.

### Recommended workflow: source-line debugging

1. **Build, then flash** — Terminal → Run Task → *Zephyr: Build (pristine)*, wait for it
   to finish, then Terminal → Run Task → *Zephyr: Flash*. (Skip this if the board is
   already running the binary you want to debug.)
2. **Start the GDB server** — Terminal → Run Task → *Zephyr: Start GDB Server (pyOCD)*.
   This runs `pyocd gdbserver` in its own dedicated terminal panel and stays running.
   Wait until that terminal prints `GDB server listening on port 3333` before moving on.
3. **Set a breakpoint** — open one of the app sources and click in the gutter on a line
   *inside* a function body — e.g. a command handler in `app/src/cmd_parser.c`, or a line
   inside a thread's `while (1)` loop body (`cmd_parser.c`, `led_ctrl.c`). Don't put it on
   the `while (1) {` line itself — that only ever fires once per debug session (see
   [Known issues](#known-issues)).
4. **Attach** — open the Run and Debug panel (`Ctrl+Shift+D`), pick *Zephyr: Debug
   (attach to running GDB server) - start server task first!* from the dropdown, and
   press F5. This config's `postAttachCommands` runs `monitor reset halt` automatically,
   so the CPU starts from the reset vector instead of attaching mid-sleep-cycle (see
   [Known issues](#known-issues) for why that matters).
5. Execution stops at the reset vector; press **Continue** (F5) to run until your
   breakpoint hits. From there, step/inspect variables/watch expressions as normal.
6. When done, stop the debug session (Shift+F5) and separately stop the *Zephyr: Start
   GDB Server (pyOCD)* task (trash-can icon in its terminal panel, or `Ctrl+C` in it) —
   only one `pyocd gdbserver` can hold the USB probe at a time.

Full root-cause writeups for the Cortex-Debug/SWD quirks this workflow works around:
`RUNBOOK.md` → Step 11c (debug config) and 11e (plain command-line GDB, also useful
outside VS Code).

## Reproducing this setup elsewhere

```powershell
git clone https://github.com/zabooh/pic32cm-pl10-cnano-zephyr.git C:\zw2
Set-Location C:\zw2
powershell -ExecutionPolicy Bypass -File reproduce-install.ps1
```

`cd` into the clone first and call the script by its bare filename (not a full path to
some other copy) — it determines its target directory from `$PSScriptRoot`, the actual
location of the `.ps1` file being run, so everything it builds resolves to *that* clone.

The script clones the pinned Zephyr revision, fetches only the four modules this board
needs, installs the pinned SDK and Python packages, builds the app, and flashes the
board — finishing with `Reproduction complete.` in green. Expect roughly 10-20 minutes
depending on network speed.

**Two things the script does *not* do for you:**
- **Prerequisites** — Python (3.12+), Git, Ninja, CMake, and 7-Zip must already be on
  `PATH`. `reproduce-install.ps1` checks for them and stops with a clear error if one is
  missing; it does not install them. Run **`install-prerequisites.ps1`** first to
  auto-install (via winget) the pinned version of any that are missing — already-present
  tools are left untouched. Open a new shell afterwards so newly added `PATH` entries take
  effect.
- **Board connection** — the board must be plugged in via USB before the script reaches
  its last step (`pyocd flash`). Everything before that succeeds without it attached.

If you're copying straight from this machine or a network share instead of cloning, copy
`reproduce-install.ps1`, `requirements-lock.txt`, and `app\` into an empty target
directory yourself, then run the script the same way from there — it does not fetch or
copy those two items itself, they must already sit next to it.

Everything here is version-pinned — the exact Zephyr commit, module revisions, SDK
version, and every Python package — so two runs on different machines produce the same
toolchain, not whatever happens to be "latest" that day. Nothing auto-updates; see
[Design decisions](#design-decisions) for how to move a pin forward deliberately.

Full detail on prerequisites and exactly what's pinned and why: `RUNBOOK.md` →
"Checking prerequisites" and Steps 1-10.

## Design decisions

Goals this workspace optimizes for:

- **Reproducibility** — one pinned script, same result every time
- **Minimal footprint** — the leanest module/toolchain set that still builds
- **No unnecessary Zephyr subsystems** — the shell was replaced with a minimal command
  parser (see [Memory usage](#memory-usage))
- **VS Code support** out of the box
- **Easy portability** to another machine

How that's achieved:

- **Shallow clone** (`--fetch-opt=--depth=1`) — no full Git history for Zephyr or its modules.
- **`manifest.group-filter`** excludes coarse optional groups (`optional`, `babblesim`, `ci`).
- **`manifest.project-filter`** (the important one) is an explicit allow-list —
  `-.*,+hal_microchip,+cmsis,+cmsis_6,+picolibc` — so only the four modules this board
  actually needs are cloned, instead of the dozens of unrelated HALs, crypto/TEE, and RTOS
  extension modules that `west update` would otherwise pull in by default.
- **SDK**: ARM-only toolchain (`-t arm-zephyr-eabi`), no bundled host tools (`-H`) since
  flashing goes through pyOCD, not OpenOCD/QEMU.
- **Python deps**: only `zephyr/scripts/requirements-base.txt` plus `pyocd` — not
  Zephyr's full test/compliance requirement set.
- **The firmware itself**: no shell subsystem — a hand-rolled command parser on
  `console_getchar()` instead, with a `pl10:~$ ` prompt, `help`, and bash-style Up/Down
  history for UX parity at a fraction of the shell's RAM.

Net result: a full, working toolchain and firmware in a few hundred MB instead of the
several GB a default Zephyr installation would use.

**Moving a version pin forward:** this is a deliberate, occasional step, not a routine
`west update` — it can drag along module revision changes and SDK-compatibility shifts,
so it needs its own end-to-end test (build + flash + interaction). `west update` only
ever syncs the four module projects to whatever `zephyr/`'s currently-checked-out
`west.yml` pins them to — it never touches `zephyr/` itself, which is pinned to one exact
commit. How to actually do it safely: [Updating the Zephyr version](#updating-the-zephyr-version-moving-the-pin)
below; the mechanism behind it: `RUNBOOK.md` → Appendix B.

## Updating the Zephyr version (moving the pin)

This workspace is pinned to one exact Zephyr commit (`$ZEPHYR_REV` in
`reproduce-install.ps1`) so builds are reproducible and don't break when upstream moves.
When you *do* want to move forward on purpose — typically to pick up newer PIC32CM PL10
support Microchip has merged into Zephyr mainline — there's a guided procedure for it:
**[`RUNBOOK-update-zephyr-pin.md`](RUNBOOK-update-zephyr-pin.md)**.

Why a runbook instead of "just change the hash": editing `$ZEPHYR_REV` alone only affects a
*fresh* from-scratch reproduction, and it silently skips several things that routinely bite.
`west update` never touches `zephyr/` itself (so the pin has to be moved by hand); a new
driver may need a module the `project-filter` currently excludes; a newer Zephyr can demand
a newer SDK or west; and RAM/flash headroom on this 8 KB part is thin. The runbook walks all
of that, in order.

It's written as an **agent-driven interview** — the intended way to run it is to let Claude
Code drive it inside VS Code. Open the workspace and say something like *"update the Zephyr
pin"* (or *"lese RUNBOOK-update-zephyr-pin.md"*). Claude then:

1. **Asks you for the target commit or tag** (and why) — it won't guess one or silently grab
   `origin/main`.
2. **Moves the pin in your live workspace** — `git checkout <commit>` inside `zephyr/`, then
   `west update`, which pulls the matching `hal_microchip`/module revision bumps.
3. **Works through the pitfalls** — widening `project-filter` if a new module is needed,
   bumping the SDK/west if the newer Zephyr requires it, a pristine rebuild, a
   `ram_report`/`rom_report` headroom check, a flash + serial-console test, and an ISR-stack
   re-test if the new code adds interrupt sources.
4. **Persists the pin only after it built and flashed** — writes the new `$ZEPHYR_REV` (plus
   any SDK/west/filter changes) back into `reproduce-install.ps1`, so the next reproduction
   matches what was just verified.

**The rule the runbook enforces:** never commit a pin you haven't actually built, flashed,
and exercised on the board. Two practical notes: you need a *reproduced* workspace to run it
in — a bare `git clone` has no `zephyr/` yet, so run `reproduce-install.ps1` once first — and
you can of course follow the steps by hand instead of via Claude; the file reads as a plain
checklist either way.

## Peripheral support status

How much of the PL10 does Zephyr actually drive today? This is a **snapshot of the pinned
Zephyr revision** (derived from the SoC devicetree plus the drivers present in the tree) —
it's exactly the kind of thing that moves forward when you
[update the pin](#updating-the-zephyr-version-moving-the-pin). Two categories matter, and
the difference is important:

**A — wired up for the PL10 (a devicetree node *and* a matching driver exist).** These work
today; the ones marked "configure" only need a Kconfig/devicetree opt-in, the driver is
already there:

| Peripheral | Driver | Status here |
|---|---|---|
| Clock / oscillators (GCLK, MCLK, OSC32K, OSCHF, XOSC32K) | `clock_control` (MCHP PIC32CM_PL) | ✅ active |
| GPIO (PORT) | `gpio_mchp_port_g1` | ✅ active (the LED) |
| Pin control (PORT) | `pinctrl_mchp_port_g1` | ✅ active |
| UART (SERCOM) | `uart_mchp_sercom_g1` | ✅ active (the console) |
| SPI (SERCOM) | `spi_mchp_sercom_g1` | 🟡 driver present, configure to use |
| I²C (SERCOM) | `i2c_mchp_sercom_g1` | 🟡 driver present, configure to use |
| DMA | `dma_mchp_dmac_g2` | 🟡 node + driver present, configure to use |

The PL10 exposes **two SERCOM instances** (sercom0/1); each can be a UART **or** SPI **or**
I²C. So SPI and I²C are only a devicetree/Kconfig change away — no new driver needed.
(An RTC node exists as part of the clock subsystem, but using it as a general-purpose
calendar/counter isn't clearly wired in this revision — treat it as uncertain.)

**B — a driver exists in the Zephyr tree, but there is no PL10 devicetree node**, so it is
*not* usable on the PL10 without doing the work yourself (see the next section):

| Peripheral | Generic driver in tree | Why not on the PL10 |
|---|---|---|
| **ADC** | `adc_mchp_g1` | no PL10 ADC node — *and* the PL10 ADC register map doesn't match `adc-g1` anyway. This is exactly why [`pl10_adc.c`](app/src/pl10_adc.c) pokes registers directly. |
| **Timers / PWM** (TC, TCC) | `tc_g1_pwm`, `tcc_g1_pwm`, `*_counter` | no TC/TCC node in the PL10 devicetree, so **hardware PWM isn't available out of the box** — a precise square wave (e.g. 1 kHz) means bridging a TC timer directly for now. |
| DAC | `dac_g1`, `dac_g2` | no PL10 node |
| Watchdog | `wdt_g1` | no PL10 node |
| Analog comparator | `ac_g1_comparator` | no PL10 node |
| TRNG / entropy | `trng_g1_entropy` | no PL10 node |
| **GPIO pin interrupts** (EIC) | `intc_mchp_eic_g1` | no EIC node — so `gpio_pin_interrupt_configure()` returns `-ENOTSUP`; the SW0 button (`button.c`) polls instead |

**In short:** clock, GPIO, pin control and UART are ready to go, with SPI/I²C/DMA a short
configuration step away; the analog and timing blocks (ADC, DAC, TC/TCC/PWM, comparator)
have no PL10 devicetree node yet and are only reachable through the bridging pattern below.
Because this tracks upstream, expect the "category B" list to shrink over time — which is
what the [pin update](#updating-the-zephyr-version-moving-the-pin) is for.

## Bridging a peripheral before Zephyr supports it

Sometimes the chip has a peripheral that Zephyr mainline doesn't drive yet — no
`*_driver_api`, no devicetree binding, nothing to `#include`. You still want to use it now.
On the PL10 the **ADC** is exactly this case, and this workspace handles it with a
deliberate pattern worth reusing: a small **stopgap driver** that pokes the hardware
registers directly today, structured so that swapping in the *real* Zephyr driver later
touches **only one file**. This section walks that pattern using the ADC
([`app/src/pl10_adc.c`](app/src/pl10_adc.c) / [`.h`](app/src/pl10_adc.h)) as the worked
example.

### Why not just wait, or fork Zephyr?

Three options, and why the middle path here is the pragmatic one:

- **Wait for upstream support** — blocks your project on someone else's release schedule.
- **Patch `zephyr/`** to add the driver and a devicetree node yourself — but this workspace
  pins a third-party Zephyr to one exact commit and treats everything outside `app/` as
  read-only (see [Design decisions](#design-decisions)). Editing the pinned tree fights the
  reproducibility the whole setup is built on, and your patch evaporates on the next clone.
- **Bridge it inside `app/`** — a self-contained module that accesses the peripheral's
  registers directly, hidden behind an ordinary interface. Nothing outside `app/` changes,
  so reproducibility is intact. This is what the ADC module does.

For the PL10 ADC specifically, the direct route was the *only* option without forking:
its register map matches neither in-tree Microchip ADC driver (`adc_mchp_g1`, `adc_sam0`),
and with no ADC devicetree node there's nothing for Zephyr's `clock_control`/`pinctrl` to
hang off either.

### The pattern — five rules that keep the later swap cheap

The entire point is **isolation**: concentrate everything hardware-specific in one module,
behind a public interface that reveals nothing about how it's implemented.

1. **Two layers in one module, one stable interface.** `pl10_adc.c` has a low-level
   *register driver* (`pl10_adc_init()`, `pl10_adc_read()`) and, on top, an *application
   service* (`pl10_adc_read_once()`, the streaming thread, the `adc` console command). Only
   the service layer is exported — [`pl10_adc.h`](app/src/pl10_adc.h) declares
   `pl10_adc_read_once()` / `pl10_adc_stream_set()` and nothing about registers. Callers
   (`cmd_parser.c`, `diag.c`) never learn it's a stopgap, so when the internals change they
   don't.
2. **Get register definitions from the HAL pack header, not hand-typed addresses.** The
   module includes `pic32cm6408pl10048.h` from the `hal_microchip` module — the same
   register/bitfield definitions the CMSIS world uses (`ADC0_REGS->ADC_CTRLA`,
   `MCLK_APBCMASK_ADC0_Msk`, …). No magic numbers, and it stays readable next to the
   datasheet.
3. **Cross-verify every hardware fact against independent sources.** The clock enable
   (`MCLK.APBCMASK` bit 7, no GCLK channel needed) and the pin (**AIN29 / PA29**, the one
   ADC-capable pin this board breaks out) were each confirmed against *three* sources that
   agree: the datasheet (DS40002667A §11.6.8, §33.4.2.5), a bare-metal CMSIS reference, and
   Microchip's official Harmony reference app for this exact board
   (`csp_apps_pic32cm_pl10` v1.0.0). Register-poking has no compiler to catch a wrong bit —
   the cross-check is the safety net.
4. **Never clobber what Zephyr already owns.** The clock bit is set with `|=`, not a plain
   assignment, so it doesn't wipe bits Zephyr's own `clock_control` driver set for other
   peripherals in the same register; the pin-mux write masks a single nibble so it leaves
   the neighbouring pin alone. A stopgap shares silicon with the RTOS — touch only your bits.
5. **Mark it loudly as a stopgap.** The header comment says what it is, why direct access
   was necessary, and *"replace this with a proper Zephyr driver once upstream support
   lands."* `CLAUDE.md` repeats it. The exit strategy is documented before it's needed.

One caveat this pattern carries: your stopgap doesn't get the real driver's built-in
locking. The ADC registers are shared by two threads (the `adc` command and the streaming
thread) with no mutex. It's safe *in practice* only because a full conversion takes
microseconds — far shorter than the 20 ms round-robin time slice (see
[Interrupts and the system tick](#interrupts-and-the-system-tick)) — and each thread
`k_sleep`s between reads, so the scheduler practically never switches threads
mid-conversion. It is not *guaranteed* safe, which is exactly what the real Zephyr driver
handles for you (`adc_context` serializes access). If you bridge a peripheral touched from
multiple threads, either add a small `K_MUTEX` or stay conscious of that assumption.

### What it looks like from the outside

Nothing about the stopgap leaks to the user. Over the serial console it's just:

```
pl10:~$ adc read
adc: 3676 (2.962 V)
pl10:~$ adc stream start
adc: streaming every 500 ms
```

(On the bare board AIN29 floats, so the value sits noisy near the supply rail — that's
expected, it confirms the ADC is actually converting.)

### Migrating to the real driver, when it lands

Upstream support arrives by moving the Zephyr pin forward
([Updating the Zephyr version](#updating-the-zephyr-version-moving-the-pin)) to a commit
that ships both an ADC driver for this IP and an ADC devicetree node. Then the migration is
contained to `pl10_adc.c` — the header and every caller stay put:

| Stopgap today (direct registers) | After (standard Zephyr ADC API) |
|---|---|
| `pl10_adc_clock_enable()` | **gone** — the driver clocks itself via `clock_control` |
| `pl10_adc_pinctrl_enable()` | **gone** — pin comes from `pinctrl` in the devicetree |
| `pl10_adc_init()` (CTRLA…E register writes) | `ADC_DT_SPEC_GET(...)` + `adc_channel_setup_dt()` |
| `pl10_adc_read()` (INPUTCTRL/COMMAND/poll) | `adc_read_dt()` into an `adc_sequence` buffer |
| `counts * 3300 / 4095` | `adc_raw_to_millivolts_dt()` (reads ref/resolution from DT) |
| `#include <pic32cm6408pl10048.h>`, `ADC0_REGS` | **gone** — no HAL pack header needed |

Concretely:

1. `CONFIG_ADC=y` in `prj.conf` (the specific driver Kconfig auto-selects once the node is
   enabled).
2. A board overlay enabling the ADC node and describing the AIN29 channel — roughly:
   ```dts
   &adc0 {
       status = "okay";
       #address-cells = <1>;
       #size-cells = <0>;
       channel@1d {                       /* 0x1d = 29 */
           reg = <29>;
           zephyr,gain = "ADC_GAIN_1";
           zephyr,reference = "ADC_REF_VDD_1";
           zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;
           zephyr,resolution = <12>;
       };
   };
   ```
   > The exact binding/property names are whatever the upstream driver defines — treat the
   > snippet above as the *shape*, not the final text, until the driver actually lands.
3. Rewrite the low-level layer per the table above; leave `pl10_adc_read_once()`,
   `pl10_adc_stream_set()`, the streaming thread, and the `adc` command untouched.
4. Build, flash, and confirm `adc read` / `adc stream` behave identically — then re-check
   `ram_report`/`rom_report`: the ADC subsystem plus a full driver usually costs more RAM
   than the tiny register poker, which matters on this 8 KB part.
5. Delete the "stopgap" notes in `CLAUDE.md` and this README.

Because the interface never changed, the concurrency caveat also disappears for free — the
real driver serializes access internally.

### Reusing the pattern for another peripheral

Same recipe, any unsupported peripheral: **(1)** one module, a register driver under an
application interface; **(2)** definitions from the `hal_microchip` pack header;
**(3)** cross-verify clock/pin/register facts against the datasheet *and* a second source;
**(4)** `|=` / masked writes so you never clobber Zephyr's bits; **(5)** a header comment
naming it a stopgap with the swap-out plan. Do that, and adopting the eventual upstream
driver is a one-file change, not a rewrite.

## Interrupts and the system tick

Zephyr sits on the Cortex-M0+ **NVIC** (Nested Vectored Interrupt Controller): each
peripheral IRQ has a number, an enable bit, and a priority, and the CPU vectors to a
handler in hardware. Zephyr routes every IRQ through a shared `_isr_wrapper` that looks the
real handler up in a generated **software ISR table** (`_sw_isr_table[]`), which is what
`IRQ_CONNECT()` (usually fed from the devicetree `interrupts` property) fills in. Drivers
register their own ISRs this way — the app itself connects none.

### How many interrupts are actually active here

Exactly **one** peripheral interrupt. The generated table has 16 slots; 15 point at
`z_irq_spurious` (unused). The only connected entry is:

| Source | IRQ | Why | Handler |
|---|---|---|---|
| **SERCOM1 (console UART)** | 8 | receives each typed character (RX), the only event-driven peripheral | `uart_mchp_sercom_g1.c` |

Everything else the app does needs **no** interrupt: the **ADC** is read by busy-polling
its ready flag (see [Bridging a peripheral…](#bridging-a-peripheral-before-zephyr-supports-it)),
and the **LED** is a GPIO *output*. `SERCOM0` exists in the SoC devicetree but no handler is
connected, so its slot stays spurious.

On top of that one peripheral IRQ run the always-on **system exceptions**, which aren't in
the NVIC table but are what make the RTOS tick:

- **SysTick** — the kernel time source (below).
- **PendSV** (lowest priority) — performs the actual thread context switch.
- **SVC** — kernel supervisor calls.

So the whole interrupt surface is effectively **UART + SysTick**. That is why the shared ISR
stack (`CONFIG_ISR_STACK_SIZE`, 1024 B) runs at only ~⅓ used with lots of headroom — there
is almost nothing competing on it. **Add an interrupt-driven peripheral later** (a real ADC
driver with a conversion-done IRQ, UART DMA, a timer capture…) and it gains a slot — at
which point the ISR-stack stress test in `CLAUDE.md` becomes mandatory again.

**No GPIO pin interrupts here.** One consequence of this minimal surface: the SW0 user
button (`button.c`) can't fire an interrupt on press. On this SoC a GPIO edge is routed
through the External Interrupt Controller (EIC), and this Zephyr revision has no EIC
devicetree node for the PL10 — so `gpio_pin_interrupt_configure()` returns `-ENOTSUP` and the
button is polled every 50 ms instead (see [Peripheral support status](#peripheral-support-status)).

### The system tick

The kernel's sense of time comes from three settings (all visible in
`build/zephyr/.config`):

| Setting | Value | Meaning |
|---|---|---|
| `CONFIG_SYS_CLOCK_TICKS_PER_SEC` | **10000** | tick resolution = **100 µs** — the finest granularity for `k_sleep`/timeouts |
| `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC` | **24000000** | the SysTick counter itself runs at 24 MHz |
| `CONFIG_TICKLESS_KERNEL` | **y** | SysTick fires **on demand**, not 10000×/s |

The important nuance is **tickless**: the 10 kHz is a *resolution*, **not** an interrupt
frequency. SysTick is reprogrammed to interrupt only when the next timeout is actually due,
so a `k_sleep(K_MSEC(500))` produces **one** SysTick interrupt ~500 ms later, not 5000
periodic ticks. This keeps the timer's share of the (already tiny) interrupt load down and
lets the core stay in `WFI` idle between events.

**Scheduling between equal-priority threads** is round-robin: `CONFIG_TIMESLICE_SIZE = 20`
(ms), applied to all preemptible threads (`CONFIG_TIMESLICE_PRIORITY = 0`). All three app
threads run at priority 7, so they time-slice against each other every 20 ms — but since
each does a little work and then `k_sleep`s, none ever burns a full 20 ms slice in practice.

## The driver model behind a pin toggle

A one-liner like `gpio_pin_toggle_dt(&led)` looks trivial, but it fans out through several
layers of Zephyr's **driver model** before a register is written. Traced in this build:

```
gpio_pin_toggle_dt(spec)                inline   — takes spec->port, spec->pin (from devicetree)
  └─ gpio_pin_toggle(port, pin)         inline
      └─ z_impl_gpio_port_toggle_bits   inline   — BIT(pin)
          └─ api->port_toggle_bits(...)  ← indirect call through the driver's API table
              └─ gpio_mchp_port_toggle_bits          (the PORT driver)
                  └─ regs->PORT_OUTTGL = pins;       ← the actual register write
```

**Most of that collapses.** The `_dt` / `pin_toggle` / `z_impl_*` wrappers are all
`static inline` and fold together at `-Os`. What remains is **one** real runtime
indirection: `api->port_toggle_bits`, a call through a **function pointer** in the driver's
API struct (`.port_toggle_bits = gpio_mchp_port_toggle_bits`). The compiler can't inline
past it, because it doesn't know at build time which driver sits behind the `struct device`.
That pointer table is how Zephyr does polymorphism in C — effectively a vtable — and it is
what makes drivers pluggable.

Why the layering earns its keep:

- **Portability** — `gpio_pin_toggle_dt()` is identical on every board; the SoC-specific
  register poke hides behind the function pointer. Change the board, the app code doesn't.
- **Devicetree binding** — the `_dt` variants resolve the device and pin at compile time
  (`spec->port` / `spec->pin` are constants).
- **`__syscall`** — `gpio_pin_toggle` is a syscall, so the same call can trap into the
  kernel under userspace (MPU) protection. This board has **no userspace** (no MPU on the
  M0+), so it compiles straight through to `z_impl_*` at zero extra cost; enabling userspace
  would add exactly one more layer (the syscall trap).

Net cost: a little argument setup, one indirect branch, and the (inlined) register write —
tiny. But it isn't the single `str` a direct register access would be, and that shows up in
two places already discussed:

- **Hot loops.** Toggling in a tight loop (say a 1 kHz square wave) pays this dispatch every
  iteration — one more reason a hardware **PWM/TC** (see
  [Peripheral support status](#peripheral-support-status)) or a direct `PORT_OUTTGL = mask`
  beats calling `gpio_pin_toggle` there. For the 500 ms blink it's utterly irrelevant.
- **Fault post-mortems.** This inline chain is exactly what `tools/faultloc.py` unwinds with
  `addr2line -i` when a fault lands inside one of these folded-together calls.

## Idle, WFI, and CPU load

When no thread is ready to run, Zephyr runs the **idle thread**, which executes a **`WFI`**
(Wait For Interrupt) — so on this firmware the core is **halted the vast majority of the
time**. The pattern is always the same: wake on an interrupt → do a few microseconds of work
(toggle the LED, read the button, handle a console character) → go back to `WFI`. The button
watcher polls SW0 every 50 ms, so the core wakes at least ~20×/s and sleeps in ≤50 ms chunks
(without that poller the 500 ms blink alone would let it sleep ~500 ms at a stretch); either
way the "actually computing" duty cycle stays well under 1 %, since each wake is only
microseconds.

**"Halted" is a light sleep, not "off":**

- The core clock is gated (no instructions execute → low power), but the **NVIC, the 24 MHz
  system-timer counter, and the peripherals keep running** — which is why the next interrupt
  reliably wakes the core, and why the load measurement below is accurate.
- It's plain `WFI`. Deeper power states (standby, clock/voltage scaling) would need Zephyr's
  power-management subsystem, which this minimal build does not enable.
- This is the *healthy* RTOS state — doing nothing efficiently. A classic superloop with a
  busy-wait delay would instead spin the CPU at 100 % to achieve the same "nothing," at full
  power. Tickless (see [Interrupts and the system tick](#interrupts-and-the-system-tick)) is
  what lets the core stay asleep for hundreds of ms instead of waking 10 000×/s just to tick.

**You can measure CPU load from that idle time.** The load is simply:

> CPU load = 1 − (time in the idle thread ÷ total time)

Enable `CONFIG_THREAD_RUNTIME_STATS` (it pulls in `CONFIG_SCHED_THREAD_USAGE`) and the kernel
timestamps every context switch, accumulating per-thread execution cycles. Read them with
`k_thread_runtime_stats_all_get()` — total cycles, plus idle cycles directly when
`CONFIG_SCHED_THREAD_USAGE_ALL` is set; two reads a second apart give a *rolling* load rather
than a since-boot average.

The subtlety worth knowing: since the idle time is spent in `WFI`, the measurement only works
if the counter keeps running while the core sleeps. It does — the runtime stats use the
24 MHz system-timer counter, which `WFI` doesn't stop, so the sleep time is correctly charged
to the idle thread. (A cycle counter that halted on `WFI` — some DWT counters do — would lose
the sleep and report a falsely high load.)

On this firmware that reading would sit **near 0 %** — the threads sleep almost always — which
matches everything else here: tiny interrupt surface, lots of stack/ISR headroom. A `load`
console command alongside `threads` would be a natural next diagnostic: same idea, CPU instead
of stack.

### How much power that WFI actually saves

The PL10 datasheet gives current figures at exactly this board's operating point — 24 MHz
from the internal OSCHF, which is what `WFI` puts into *Idle* sleep mode (3.0 V, typical):

| Mode | Current | What it is here | Source |
|---|---|---|---|
| **Active** (`while(1)` loop) | **5.2 mA** | a 100 %-busy superloop | [Table 37-6](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-40CFB883-5F42-451C-89CD-2C397F94A042.html) |
| **Idle** (`WFI`, CPU stopped, clocks run) | **1.2 mA** | our idle thread | [Table 37-7](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-D4489DC3-1212-49E1-98F8-A8DFAB8A5D61.html) |
| **Standby** (ULP regulator) | **2.0 µA** | unused (needs Zephyr PM) | [Table 37-8](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-5D2F58F6-00E1-4CE4-8EE2-A58643CF63E7.html) |

The average current is `I = d·5.2 mA + (1 − d)·1.2 mA`, where `d` is the compute duty cycle.
Since the threads sleep almost always (`d` ≪ 1 %), the firmware sits essentially at the idle
figure, **~1.2 mA, versus ~5.2 mA if it busy-looped** — roughly **4 mA / ~77 % saved** just
from letting the idle thread `WFI` instead of spinning. In power terms (~3.3 V board rail):
about **17 mW → 4 mW**.

The bigger prize is untapped: `WFI` (Idle) only reaches 1.2 mA, but the chip's **Standby**
mode is **2.0 µA** — another ~600× lower. Getting there needs Zephyr's power-management
subsystem (`CONFIG_PM`, not enabled in this minimal build) and costs ~120 µs of wake latency
([Table 37-10](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-2CE6F43E-C56B-435D-8192-75DB547C378B.html))
instead of waking instantly. For a battery design that's the real lever; `WFI` is just the
free half of it.

Two honest caveats: these are **MCU-core typicals** with peripherals off, so the real board
(UART/OSCHF/GPIO active) draws a bit more; and you **can't read them at the USB port** — the
on-board nEDBG debugger and the LED draw their own current and dominate that measurement. To
see the core figure you'd measure the MCU rail directly (the Curiosity Nano has a cut-strap /
measurement point for exactly this).

**Why `CONFIG_PM` isn't the answer here (yet).** You might expect to reach that 2 µA Standby
by enabling Zephyr's power-management subsystem — but in this pinned revision the PL10 has no
PM support to drive: there is no SoC `pm_state_set()` implementation under
`zephyr/soc/microchip/pic32c/`, and the devicetree defines no `power-states` for the CPU to
enter. So `CONFIG_PM=y` would have no low-power states to pick and would gain nothing over the
plain `WFI` idle you already have. Crucially, **no `CONFIG_PM` ≠ no power saving** — the
default idle already does `WFI` (the Idle-mode tier); what's missing is the *automatic* drop
into Standby. Reaching Standby today means implementing it yourself — a `power-states`
devicetree overlay plus a `pm_state_set()` that writes `SLEEPCFG.SLEEPMODE` (the same
"bridge it in your own code" pattern as the ADC, only closer to the SoC and trickier, since it
also involves wake sources and the `SUPC` regulator) — or waiting for upstream PM support to
land via a [pin update](#updating-the-zephyr-version-moving-the-pin).

### How low could it actually go?

The floor is the chip's **Standby** current, ~2 µA. The full ladder at 24 MHz (datasheet
typicals):

| State | Current | vs. today |
|---|---|---|
| Active (busy loop) | 5.2 mA | — |
| **Idle / WFI (today)** | **1.2 mA** | where you are |
| **Standby**, everything off (ULP regulator) | **2.0 µA** | ~600× below Idle |

The bare 2 µA assumes *nothing* is running — but then nothing can wake you either. A useful
"sleep, wake periodically" design adds the quiescent currents of the always-on blocks
([Table 37-9](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-1D8AF548-A9EA-4FDE-96D4-862F8673CFC7.html)):

| Block | Current | For |
|---|---|---|
| Standby base | 2.0 µA | core + SRAM retained |
| + RTC | 0.5 µA | timed wake-up |
| + OSC32K | 0.2 µA | 32 kHz clock for the RTC |
| + BOD (32 Hz sampling) | ~0.6 µA | brown-out protection (optional) |

→ **~2.7 µA** with an RTC wake, **~3.3 µA** if you keep brown-out sampling on. Blocks you
must leave *off* to get there: analog comparator (25 µA), ADC reference (190 µA), the ADC
itself (1.7 mA).

So the full span is **5.2 mA → 1.2 mA → ~2–3 µA** — deep Standby is ~400–600× below today's
`WFI` idle. For perspective, ~3 µA average would run for **years** off a CR2032 (~220 mAh)
coin cell, versus roughly a **week** at today's 1.2 mA continuous `WFI`. All of this is the
*silicon* floor, though: it needs the Standby implementation that doesn't exist yet (above),
the figures are room-temperature typicals, and the Curiosity Nano board itself (debugger, LED)
draws more — a real µA design needs a board built for it.

**What the button watcher costs.** `button.c` polls SW0 every 50 ms, so the core now wakes
~20×/s instead of ~2×/s. In *this* build that barely moves the average current: `WFI` draws
its 1.2 mA continuously whether the core sleeps in 50 ms or 500 ms chunks, and the extra wakes
are only microseconds of compute — so it stays ~1.2 mA. Where it *would* bite is a Standby
design: waking 20×/s can't reach the ~2 µA floor, pushing the average into the tens of µA and
erasing most of the Standby benefit. Getting both the button *and* µA sleep would need an
**event-driven (interrupt) button** so the core only wakes on a real press — but that needs
the EIC, which the PL10 lacks in this Zephyr (see
[Peripheral support status](#peripheral-support-status)), which is exactly why `button.c`
polls instead.

## Memory usage

This board has only 8 KB RAM / 60 KB flash, so usage is worth watching, not just "does it
fit." Current build (LED blink + ADC + command console with history + a button watcher +
thread introspection, six threads):

| | Used | Capacity | % used |
|---|---|---|---|
| RAM | 5,232 B | 8,192 B | **63.87%** |
| Flash | 19,128 B | 61,440 B | **31.13%** |

Check live numbers with `west build -d C:\zw\build -t ram_report` / `-t rom_report`
(per-symbol breakdown) — or, on the running board, the **`threads`** console command,
which prints each thread's live stack high-water mark.

Most of the RAM is the four application threads — each `k_thread` costs its stack (352 B
blink, 352 B ADC stream, 640 B console, 384 B button) plus a 120 B thread control block, all
directly visible in `ram_report`. Those stack sizes live in one place (`app_threads.h`) and were
set with headroom over the measured high-water marks — the `threads` command flagged the
first cut (256/320 B) as running at 100%/97%, so they were bumped. As a runtime safety
net, `CONFIG_STACK_SENTINEL` is enabled (this MPU-less Cortex-M0+ can't use hardware stack
protection): if a thread ever overflows anyway, the kernel raises a fatal error naming it
instead of silently corrupting memory. Fatal errors (a stack overflow, or a HardFault from
a bad `mem` address) print their dump and then auto-reboot to the prompt
(`CONFIG_RESET_ON_FATAL_ERROR`) rather than hanging. The rest is kernel scaffolding, itself trimmed
(`ISR_STACK_SIZE` 2048→1024, `MAIN_STACK_SIZE` 1024→512).
Staying this lean is deliberate: no Zephyr shell subsystem (a hand-rolled
`console_getchar()` parser instead, which alone saved the shell's ~40-47% of RAM), only
the four HAL/lib modules this board needs, `CONFIG_LOG=n`.

A bare blinky earlier in this project's history sat at just 2,744 B (33.50%). Full
version-by-version numbers, the per-module `ram_report`/`rom_report` breakdown, a
comparison against a hand-tuned CMSIS/RTX5 app on the same chip, and the measured
per-thread RAM cost: `RUNBOOK.md` → Appendix A (those figures predate the ADC/threads/
history work — this section has the current totals).

## Known issues

- **pyOCD 0.44.1 is broken for this board.** Flashing/connecting fails with `SWD/JTAG
  communication failure (FAULT ACK)` / `(No ACK)` regardless of connect mode or clock
  speed. Stay on the pinned `pyocd==0.43.0`; re-pin if you ever
  `pip install --upgrade pyocd` and flashing suddenly breaks.
- **A missing Zephyr SDK fails hard everywhere, never silently.** `reproduce-install.ps1`
  aborts at its `west sdk install` step; a manual `west build` without the SDK fails at
  CMake's configure step with "could not find Zephyr SDK"; VS Code debugging/IntelliSense
  fail to find `arm-zephyr-eabi-gdb.exe`/compiler include paths. Nothing produces a
  half-finished result.
- **Calling `west.exe`/`pyocd.exe` by full path without activating the venv** works for
  `west build` but not `west flash` — its pyOCD runner shells out to a bare `pyocd`
  command and relies on `PATH` to find it. Activate the venv first.
- **Cortex-Debug 1.12.1 on Windows can't reliably auto-launch its own pyOCD server.** Use
  the "start GDB server task, then attach" workflow under
  [Working in VS Code](#working-in-vs-code) instead of the two auto-launch debug configs.
  Full root cause: `RUNBOOK.md` → Step 11c.
- **Attaching to a sleeping core causes intermittent SWD `FAULT ACK` faults** — this
  board's SWD port becomes briefly inaccessible while the CPU is in `k_msleep()`-driven
  WFI idle. Fixed automatically via `monitor reset halt` in the attach configs'
  `postAttachCommands`. Also: put breakpoints inside a loop body, not on the
  `while (1) {` line itself — it only fires once. Full root cause: `RUNBOOK.md` → Step 11c.
- **`launch.json`/`c_cpp_properties.json` reference the SDK via
  `${env:USERPROFILE}/zephyr-sdk-1.0.1/...`.** This is portable across machines as-is:
  `${env:USERPROFILE}` resolves per user automatically, and the version segment matches
  the SDK the reproduction script pins and installs to the default per-user location. So
  on a normal clone-and-reproduce, **no editing is needed**. Hand-edit both files only if
  you install the SDK somewhere other than that default location, or bump the SDK version
  (in which case also update `$SDK_VER` in `reproduce-install.ps1` to match). See
  `RUNBOOK.md` → Step 11 for the full rationale and JSON.

## What is `RUNBOOK.md`?

This README documents how to *use* the workspace. `RUNBOOK.md` documents how it was
*built* — the step-by-step log the whole thing was constructed from, written as
instructions an agent actually followed (see its "Task" and "Constraints" sections at the
top), not a retrospective summary. It's longer and more detailed than this README on
purpose:

- **Steps 1-9** — scaffold the workspace, `west init`, narrow the module set down to the
  four this board needs (the `project-filter` reasoning — why `group-filter` alone can't
  do it), fetch modules shallow, export/trim the toolchain, verify the board target,
  write the app (`main.c` + `prj.conf`, with the shell-vs-`console_getline()` decision and
  numbers), build/flash, and a functional test over the serial console.
- **Step 10** — how `reproduce-install.ps1` itself was produced: harvesting the exact
  pinned values (Zephyr commit, SDK/pyOCD versions, module revisions) out of a verified
  working install, plus a bug found and fixed along the way (`west init --mr <SHA>`
  doesn't work — `git`'s `--branch` can't resolve a raw commit SHA).
- **Step 11** — the VS Code integration: why each task/debug-config/IntelliSense setting
  in `.vscode/` looks the way it does, including the Cortex-Debug and `FAULT ACK`
  debugging issues referenced in [Known issues](#known-issues).
- **Appendix A** — the full memory analysis behind [Memory usage](#memory-usage).
- **Appendix B** — the full `west update` explanation behind
  [Design decisions](#design-decisions).
- A **Troubleshooting table** keyed by symptom, and a **completion checklist**.

**Read RUNBOOK.md when:** you're about to change module filters, SDK setup, or flashing
config (CLAUDE.md says so explicitly); you hit a build/flash/debug failure this README's
[Known issues](#known-issues) doesn't already cover; or you want the *why* behind a
decision, not just the current end state.

## PIC32CM PL10 device series

This board carries the **PIC32CM6408PL10048** (64 KB flash, 48-pin VQFN), but that's one
member of a wider PL10 family. They are all the **same Arm Cortex-M0+ @ 24 MHz** silicon with
the **same peripheral set** — 12-bit 800 ksps ADC, PTC touch, 2× analog comparators, 2×
SERCOM, TC0-2 + TCC0 (PWM), 32-bit RTC, 2-channel DMAC, WDT, EVSYS/CCL, 1.8–5.5 V operation.
They differ only in **flash / SRAM size** and **pin count / package**:

| Device | Flash | SRAM | Pin/package options |
|---|---|---|---|
| PIC32CM**3204**PL10 | 32 KB | 4 KB | 20 (VQFN, SSOP), 28 (VQFN, SSOP), 32 (TQFP, VQFN) |
| PIC32CM**6408**PL10 | 64 KB | 8 KB | 28 (VQFN, SSOP), 32 (TQFP, VQFN), **48 (VQFN) ← this board** |
| PIC32CM**1216**PL10 | 128 KB | 16 KB | 28 (VQFN, SSOP), 32 (TQFP, VQFN) |

The 4-digit code encodes it: `6408` = 64 KB flash / 8 KB SRAM, `3204` = 32/4, `1216` = 128/16;
the trailing `…10NNN` is the pin count.

**Ordering-suffix key** (to read the part numbers below): temperature grade **I** = industrial
(−40…+85 °C), **E** = extended (−40…+125 °C); a **T** before the dash = tape-and-reel (else
tube/tray); package code **/2LX** = 20-pin VQFN, **/3LW** = 28-pin VQFN, **/QZB** = 32-pin
VQFN, **/PT** = 32-pin TQFP, **/SS** = SSOP, **/6MX** = 48-pin VQFN.

Full orderable part numbers (prices omitted):

```
32 KB flash / 4 KB SRAM — PIC32CM3204PL10
  PIC32CM3204PL10020-I/2LX    20 VQFN      PIC32CM3204PL10020-E/2LX    20 VQFN
  PIC32CM3204PL10020T-I/2LX   20 VQFN T/R  PIC32CM3204PL10020T-E/2LX   20 VQFN T/R
  PIC32CM3204PL10020-I/SS     20 SSOP      PIC32CM3204PL10020-E/SS     20 SSOP
  PIC32CM3204PL10020T-I/SS    20 SSOP T/R  PIC32CM3204PL10020T-E/SS    20 SSOP T/R
  PIC32CM3204PL10028-I/3LW    28 VQFN      PIC32CM3204PL10028-E/3LW    28 VQFN
  PIC32CM3204PL10028T-I/3LW   28 VQFN T/R  PIC32CM3204PL10028T-E/3LW   28 VQFN T/R
  PIC32CM3204PL10028-I/SS     28 SSOP      PIC32CM3204PL10028-E/SS     28 SSOP
  PIC32CM3204PL10028T-I/SS    28 SSOP T/R  PIC32CM3204PL10028T-E/SS    28 SSOP T/R
  PIC32CM3204PL10032-I/PT     32 TQFP      PIC32CM3204PL10032-E/PT     32 TQFP
  PIC32CM3204PL10032T-I/PT    32 TQFP T/R  PIC32CM3204PL10032T-E/PT    32 TQFP T/R
  PIC32CM3204PL10032-I/QZB    32 VQFN      PIC32CM3204PL10032-E/QZB    32 VQFN
  PIC32CM3204PL10032T-I/QZB   32 VQFN T/R  PIC32CM3204PL10032T-E/QZB   32 VQFN T/R

64 KB flash / 8 KB SRAM — PIC32CM6408PL10
  PIC32CM6408PL10028-I/3LW    28 VQFN      PIC32CM6408PL10028-E/3LW    28 VQFN
  PIC32CM6408PL10028T-I/3LW   28 VQFN T/R  PIC32CM6408PL10028T-E/3LW   28 VQFN T/R
  PIC32CM6408PL10028-I/SS     28 SSOP      PIC32CM6408PL10028-E/SS     28 SSOP
  PIC32CM6408PL10028T-I/SS    28 SSOP T/R  PIC32CM6408PL10028T-E/SS    28 SSOP T/R
  PIC32CM6408PL10032-I/PT     32 TQFP      PIC32CM6408PL10032-E/PT     32 TQFP
  PIC32CM6408PL10032T-I/PT    32 TQFP T/R  PIC32CM6408PL10032T-E/PT    32 TQFP T/R
  PIC32CM6408PL10032-I/QZB    32 VQFN      PIC32CM6408PL10032-E/QZB    32 VQFN
  PIC32CM6408PL10032T-I/QZB   32 VQFN T/R  PIC32CM6408PL10032T-E/QZB   32 VQFN T/R
  PIC32CM6408PL10048-I/6MX    48 VQFN   <-- this board's MCU

128 KB flash / 16 KB SRAM — PIC32CM1216PL10
  PIC32CM1216PL10028-I/3LW    28 VQFN      PIC32CM1216PL10028-E/3LW    28 VQFN
  PIC32CM1216PL10028T-I/3LW   28 VQFN T/R  PIC32CM1216PL10028T-E/3LW   28 VQFN T/R
  PIC32CM1216PL10028-I/SS     28 SSOP      PIC32CM1216PL10032-I/QZB    32 VQFN
  PIC32CM1216PL10028T-I/SS    28 SSOP T/R  PIC32CM1216PL10032T-I/QZB   32 VQFN T/R
  PIC32CM1216PL10032-I/PT     32 TQFP
  PIC32CM1216PL10032T-I/PT    32 TQFP T/R
```

## Porting to another PL10 device

Because the whole series shares the core, the peripherals, and Zephyr's `pic32cm_pl` HAL,
porting *within* the family is mostly board bring-up plus a target swap — **not an app
rewrite**. The app in `app/src/` reaches hardware through devicetree aliases (`led0`, `sw0`)
and the SERCOM console, so it follows the board definition rather than hard-coding the part.
How much work depends on which member you target — and, crucially, on **what Zephyr already
ships for it in the pinned revision**:

| Target | Effort | Why |
|---|---|---|
| **Another 64 KB (6408) part** — 28/32/64-pin | **Low** | Zephyr already ships the SoC devicetree (`pic32cm6408pl10028/032/048/064.dtsi`). On a custom board you write a board definition with your pins; swap `$BOARD`/`$PYOCD_TARGET`. |
| **128 KB (1216)** | **Medium** | Only the memory include (`pic32cm_1216_pl.dtsi`) exists — no per-device dtsi yet, so you'd add one (small, modelled on the 6408 files) or wait for upstream. RAM is generous (16 KB). |
| **32 KB (3204)** | **High** | No Zephyr SoC support at all in this revision (no dtsi) — you'd add it. And the blocker: **4 KB SRAM won't hold the current 5.2 KB build** — you'd first trim threads/stacks/introspection (see [Memory usage](#memory-usage)). |

Regardless of target, the mechanical changes are the same:

- **Devicetree**: pick the right SoC dtsi and write/adjust a board file for your pins (LED,
  button, the SERCOM console's pinctrl). If you keep the ADC stopgap, its hard-coded
  **AIN29 / PA29** pin must exist on your package/board or be repointed
  (see [Bridging a peripheral…](#bridging-a-peripheral-before-zephyr-supports-it)).
- **Flashing**: change the pyOCD target (`-t <part>`) — the CMSIS-DAP DFP pack must cover it —
  and update `$BOARD` / `$PYOCD_TARGET` in `reproduce-install.ps1` and the `.vscode/` configs.
- **Verify**: rebuild, re-flash, and re-check `ram_report`/`rom_report` — moving *down* in
  memory is the real constraint, moving up (to 64/128 KB) is free headroom.

The short version: **staying on a 64 KB PL10 is easy** (board file + target swap); **128 KB
needs a small devicetree addition**; **32 KB needs both SoC enablement and a RAM diet.**
Moving to a different SoC *family* (a JH, SG, CK…) would be a bigger job — different clock
tree, pinctrl, and memory map — but still the same app code behind the devicetree.
