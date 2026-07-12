# PIC32CM PL10 Curiosity Nano — Lean Zephyr Blinky

Minimal, from-scratch [Zephyr RTOS](https://www.zephyrproject.org/) workspace for the
**Microchip PIC32CM PL10 Curiosity Nano** board, plus a pinned, non-interactive script
that reproduces the entire installation — RTOS clone, HAL modules, Python venv,
toolchain, build, and flash — on another Windows machine.

## Contents

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
- [Memory usage](#memory-usage)
- [Known issues](#known-issues)
- [What is `RUNBOOK.md`?](#what-is-runbookmd)

## Executive summary

This is a blinky application — LED on/off/toggle/blink over a serial command line —
built to answer one question: **can Zephyr RTOS run comfortably on an 8 KB-RAM
microcontroller**, or does it need a beefier part to be worth using?

- Interactive serial commands: `led on/off/toggle/blink <ms>`, `help`
- Fully reproducible setup: one script rebuilds the whole toolchain (Zephyr, HAL
  modules, SDK, Python deps) from scratch on any Windows machine, pinned to exact
  versions
- Lean by construction: only the Zephyr modules this board actually needs are cloned
  (not the dozens of unrelated HALs/subsystems a default `west update` would pull in),
  keeping the whole installation **under 2 GB** instead of several GB
- VS Code integration: build/flash tasks, source-line debugging, IntelliSense
- Only **33.50% RAM / 27.13% flash** used of this board's 8 KB RAM / 60 KB flash

Where to go from here:
- Just want to build and flash it? → [Quick start](#quick-start)
- Want to reproduce this install elsewhere? → [Reproducing this setup elsewhere](#reproducing-this-setup-elsewhere)
- Want to understand *why* a decision was made? → [`RUNBOOK.md`](RUNBOOK.md)

## Key takeaways

- Zephyr RTOS runs comfortably on the PIC32CM PL10's 8 KB RAM — contrary to the common
  assumption that it needs a bigger part.
- Final footprint is 2,744 B RAM (33.50%) — smaller than a comparable hand-tuned
  CMSIS/RTX5 application on the same chip (see [Memory usage](#memory-usage)).
- The entire toolchain — Zephyr revision, HAL modules, SDK, Python packages — is
  version-pinned and reproducible with one script on a fresh Windows machine.
- Source-line debugging works reliably in VS Code once a couple of Windows-specific
  Cortex-Debug/pyOCD quirks are worked around (documented once, not re-discovered every
  time — see [Known issues](#known-issues)).
- The RAM win came specifically from replacing Zephyr's shell subsystem with a minimal
  command parser — Zephyr itself isn't the heavy part.

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

Connect a serial terminal (PuTTY, TeraTerm, ...) to the board's virtual COM port at
**115200 baud**. The prompt `pl10:~$ ` appears. Type a command and press Enter, it echoes
back, prints the response, then shows the prompt again. Try `help` for the command list:

```
pl10:~$ led on
pl10:~$ led off
pl10:~$ led toggle
pl10:~$ led blink 100
pl10:~$ led blink 1000
```

Don't have a set-up machine yet? See [Reproducing this setup elsewhere](#reproducing-this-setup-elsewhere).

## Project structure

| Path | Purpose |
|---|---|
| `app/` | The application: `CMakeLists.txt`, `prj.conf`, `src/main.c` (blinky + `led` command parser over `console_getline()`) |
| `zephyr/` | Zephyr RTOS source (shallow clone, pinned revision) |
| `modules/` | Only the HAL/library modules actually needed: `hal_microchip`, `cmsis`, `cmsis_6`, `picolibc` |
| `build/` | CMake/Ninja build output; `build/zephyr/zephyr.hex` is the flashable artifact |
| `.venv/` | Python virtual environment (west, pyOCD, build dependencies) |
| `reproduce-install.ps1` | Pinned, non-interactive script that recreates this whole setup from scratch |
| `requirements-lock.txt` | Exact pinned versions of every Python package in `.venv` |
| `.vscode/` | Build/flash tasks, debug configs, and IntelliSense settings for working in VS Code (see below) |
| `RUNBOOK.md` | Step-by-step build log with rationale, troubleshooting table, and lessons learned |

### Hardware

- Board: **PIC32CM PL10 Curiosity Nano** (Zephyr board name `pic32cm_pl10_cnano`, SoC `pic32cm6408pl10048`, Arm Cortex-M0+)
- On-board debugger: Microchip **nEDBG** (CMSIS-DAP over USB), also exposes a virtual COM port for the serial console
- No external debug probe needed — just a USB data cable

### Architecture

```
+-------------------------------------+
|  app/src/main.c                      |
|  LED command parser over             |
|  console_getline() (no shell subsys) |
+-------------------+-------------------+
                    |
                    v
+-------------------------------------+
|  Zephyr RTOS kernel                  |
|  k_timer (blink), GPIO + UART drivers |
+-------------------+-------------------+
                    |
                    v
+-------------------------------------+
|  HAL / library modules                |
|  hal_microchip, cmsis, cmsis_6,      |
|  picolibc                             |
+-------------------+-------------------+
                    |
                    v
+-------------------------------------+
|  PIC32CM PL10 (Arm Cortex-M0+)        |
+-------------------------------------+
```

## Working in VS Code

Open `C:\zw` itself as the workspace root. `.vscode/extensions.json` will prompt you to
install the two extensions this setup relies on:

- **C/C++** (`ms-vscode.cpptools`) — IntelliSense, backed by `build/compile_commands.json`
- **Cortex-Debug** (`marus25.cortex-debug`) — GDB/pyOCD debugging UI

**Build** — `Ctrl+Shift+B`, or Terminal → Run Task:
- *Zephyr: Build (pristine)* (default) — full `-p always` rebuild, also regenerates `compile_commands.json`
- *Zephyr: Build (incremental)*
- *Zephyr: Menuconfig*
- *Zephyr: Clean build dir*

**Flash** — Terminal → Run Task → *Zephyr: Flash*. Depends on the pristine build task, so
it always ships a fresh binary; picks up the board's pinned SWD frequency from
`board.cmake` automatically.

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
3. **Set a breakpoint** — open `app/src/main.c` and click in the gutter on a line
   *inside* a function body (e.g. `main.c:64`). Don't put it on `while (1) {` itself —
   that line only ever fires once per debug session (see [Known issues](#known-issues)).
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
  `PATH`. The script checks for them and stops with a clear error if one is missing; it
  does not install them.
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
- **The firmware itself**: no shell subsystem — a minimal `console_getline()`-based
  command parser instead, with a hand-written `pl10:~$ ` prompt and `help` command for UX
  parity.

Net result: a full, working toolchain and firmware in a few hundred MB instead of the
several GB a default Zephyr installation would use.

**Moving a version pin forward:** this is a deliberate, occasional step, not a routine
`west update` — it can drag along module revision changes and SDK-compatibility shifts,
so it needs its own end-to-end test (build + flash + interaction). `west update` only
ever syncs the four module projects to whatever `zephyr/`'s currently-checked-out
`west.yml` pins them to — it never touches `zephyr/` itself, which is pinned to one exact
commit. Full explanation and the 5-step procedure for moving the pin forward: `RUNBOOK.md`
→ Appendix B.

## Memory usage

This board has only 8 KB RAM / 60 KB flash, so usage is worth watching, not just "does it
fit." Current build:

| | Used | Capacity | % used |
|---|---|---|---|
| RAM | 2,744 B | 8,192 B | **33.50%** |
| Flash | 16,668 B | 61,440 B | **27.13%** |

Check live numbers with `west build -d C:\zw\build -t ram_report` / `-t rom_report`
(per-symbol breakdown).

Most of this budget was won by dropping the Zephyr shell subsystem for a minimal
`console_getline()`-based parser — the shell alone used ~40-47% of RAM for four trivial
GPIO commands — plus trimming the kernel's fixed interrupt/main stacks. The result lands
in the same ballpark as a hand-tuned CMSIS/RTX5 application on the same chip, and pulls
ahead once those kernel stacks are trimmed.

Full version-by-version numbers, the per-module `ram_report`/`rom_report` breakdown, the
CMSIS/RTX5 comparison, and the measured per-thread RAM cost: `RUNBOOK.md` → Appendix A.

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
- **`launch.json`/`c_cpp_properties.json` hardcode this machine's SDK path**
  (`${env:USERPROFILE}/zephyr-sdk-<version>/...`). Update both after reproducing this
  setup on another machine — they're not parameterized by the reproduction script. See
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
