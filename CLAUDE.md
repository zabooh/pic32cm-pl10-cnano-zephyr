# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A minimal, from-scratch Zephyr RTOS workspace (`west` topdir) for the **Microchip PIC32CM PL10 Curiosity Nano** board (`pic32cm_pl10_cnano`, SoC `pic32cm6408pl10048`, Arm Cortex-M0+). The application (`app/`) is a blinky with a minimal serial command parser (`console_getline()`, not the Zephyr shell subsystem — see Architecture below) for controlling the LED interactively. Built and documented step-by-step in `RUNBOOK.md`; `README.md` is the quick-reference summary.

This is not a general-purpose codebase — almost everything under `C:\zw` besides `app/` and the dotfiles is third-party (Zephyr source, HAL modules, venv). Only `app/`, `RUNBOOK.md`, `README.md`, `reproduce-install.ps1`, `requirements-lock.txt`, and `.vscode/` are hand-maintained.

## Commands

Activate the venv first in any new shell: `& C:\zw\.venv\Scripts\Activate.ps1` (or call `.venv\Scripts\west.exe` / `.venv\Scripts\pyocd.exe` directly by full path, which works without activation).

**Build:**
```powershell
Set-Location C:\zw\zephyr
$env:CMAKE_GENERATOR = "Ninja"
west build -p always -b pic32cm_pl10_cnano -d C:\zw\build C:\zw\app
```
`-p always` forces a pristine (clean) build — always use it after any config/filter change. Artifact: `C:\zw\build\zephyr\zephyr.hex`.

**Flash** (run from the build dir so `west flash` picks up the board's pinned SWD frequency from `board.cmake` automatically — do not guess a frequency manually):
```powershell
Set-Location C:\zw\build
west flash
```

**Menuconfig:** `west build -d C:\zw\build -t menuconfig`

**Reproduce this entire setup from scratch on another machine:**
```powershell
powershell -ExecutionPolicy Bypass -File C:\zw\reproduce-install.ps1
```
Non-interactive; pins the Zephyr commit, west/SDK/pyOCD versions, module filters, and Python deps (`requirements-lock.txt`). Ends with its own build+flash verification.

**VS Code:** open `C:\zw` as the workspace root; `.vscode/tasks.json` and `.vscode/launch.json` wrap the same build/flash/debug commands (see README.md "Working in VS Code" for the task/config names).

There is no test suite in this repo — verification is build success + flash success + manual/`pyserial`-scripted command interaction (`led on/off/toggle/blink <ms>` over the board's virtual COM port at 115200 baud, no prompt — see Architecture).

## Architecture

**`app/`** is the only application code: `CMakeLists.txt` (points `find_package(Zephyr)` at `$ENV{ZEPHYR_BASE}`), `prj.conf` (Kconfig — serial console + GPIO + `console_getline()`, `CONFIG_LOG=n`), `src/main.c` (single file: a `gpio_dt_spec` for the `led0` devicetree alias, a `k_timer` that toggles the LED on expiry (decouples blinking from command reading), and a `main()` loop that blocks on `console_getline()` and dispatches lines via `strcmp`/`strncmp` to `led on`/`off`/`toggle`/`blink <ms>` handlers — no shell subsystem).

**Why no Zephyr shell**: it was the original design (see git history / RUNBOOK.md's own revision log) but `west build -t ram_report` showed the shell subsystem alone at **~40-47% of this part's 8 KB RAM** (mostly `CONFIG_SHELL_STACK_SIZE`'s default 2048 B thread stack) for four trivial GPIO commands. Replaced with `zephyr/console/console.h`'s `console_getline()` — no dedicated thread/stack of its own, runs synchronously in the caller. A hand-written `pl10:~$ ` prompt and `help` command were added back in `main.c` (`printk` calls with static string literals, no shell needed) for UX parity — cost was +260 B flash, +0 B RAM.

**Kernel's own fixed baseline was trimmed too**: `CONFIG_ISR_STACK_SIZE` 2048→1024, `CONFIG_MAIN_STACK_SIZE` 1024→512 (`prj.conf`). `ISR_STACK_SIZE` is shared by *every* interrupt in the system (GPIO, UART RX, SysTick) plus kernel init — riskier to cut than a single thread's stack, since worst-case need is harder to bound and an overflow is more likely to silently corrupt memory than fault cleanly. Verified stable with an adversarial `pyserial` test: `led blink 10` (10 ms timer-ISR period) running concurrently with 15 rapid `help` calls (deepest `printk` stack use in the app) — no fault. This is an empirical stress test of *this app's* actual interrupt surface, not a formal worst-case proof; **re-verify with a similar stress test before trusting these values if you add new interrupt sources** (new peripheral driver, higher-priority IRQs).

Current usage: **FLASH 27.13% (16668/61440 B), RAM 33.50% (2744/8192 B)** — re-check with `ram_report`/`rom_report` after any change, this part has very little headroom to spare. (Per-thread cost if you add `k_thread`s: ~368 B each — 256 B stack + 112 B thread control block, measured directly, no hidden overhead.)

**Workspace is deliberately lean**, not a stock Zephyr install — this shapes how you should reason about missing headers/modules/build errors:
- `west config manifest.group-filter` = `-optional,-babblesim,-ci` (coarse, excludes whole manifest groups).
- `west config manifest.project-filter` = `-.*,+hal_microchip,+cmsis,+cmsis_6,+picolibc` (the real narrowing mechanism — an explicit allow-list by project name; see `RUNBOOK.md` Step 3 for why `group-filter` alone can't do this). `modules/` therefore contains only these four HAL/lib modules, not the dozens `west update` would otherwise fetch.
- If a build error is "missing header from hal_X" or similar, the fix is almost always to loosen `project-filter` for that one project (`west config manifest.project-filter -- "...,+hal_X"`) and `west update` again — not to widen `group-filter`.
- SDK is ARM-toolchain-only (`arm-zephyr-eabi`), installed without bundled host tools (`-H`, since flashing goes through pyOCD, not OpenOCD/QEMU).
- Python deps come from `zephyr/scripts/requirements-base.txt` + `pyocd`, never `west packages pip --install` / the full `requirements.txt` (pulls in unrelated test/compliance packages and can fail to build `hidapi` on Windows).

**pyOCD version is load-bearing: must stay `0.43.0`.** Version 0.44.1 cannot establish an SWD session with this board (`FAULT ACK`/`No ACK` regardless of connect mode or frequency) — confirmed independently twice on this machine. If flashing suddenly breaks with SWD/JTAG errors after any pip upgrade, check `pip show pyocd` before debugging anything else.

**Board-specific values already verified** (don't re-derive unless the board or Zephyr revision changes): board name `pic32cm_pl10_cnano`, pyOCD target `pic32cm6408pl10048`, LED alias `led0` (maps to PB2 in the board devicetree). SWD frequency: `board.cmake` defaults to `100000` Hz, but this workspace's own tooling (`.vscode/tasks.json`/`launch.json`, `reproduce-install.ps1`) explicitly overrides it to **`2000000`** Hz (verified stable — flash + multi-cycle debug session tested at 2 MHz with no faults) since `west flash` reads `board.cmake` directly and can't be patched without losing the override on a fresh `zephyr/` clone.

**Debugging this board is fault-prone unless you reset first.** The SWD port becomes intermittently inaccessible while the Cortex-M0+ core is asleep (`k_msleep`/timer-driven idle triggers WFI) — polling status while attached to an already-running, already-sleeping target reproducibly causes `SWD/JTAG communication failure (FAULT ACK)`, in both Cortex-Debug and plain `gdb`. Fix: always issue `monitor reset halt` immediately after connecting/attaching, before setting breakpoints or continuing — both attach-type `launch.json` configs do this automatically via `postAttachCommands`. A breakpoint on `while (1) {` (or a bare `break main`) also only fires once, at first loop entry — break on a line *inside* the loop body instead. Full detail: `RUNBOOK.md` Step 11c/11e.

**`.vscode/launch.json` and `.vscode/c_cpp_properties.json` reference the SDK path via `${env:USERPROFILE}/zephyr-sdk-<version>/...`** — portable across machines as long as the SDK is installed to the default per-user location (which `west sdk install` does). Only the SDK version segment is hand-pinned (matches `reproduce-install.ps1`'s `$SDK_VER`); bump it by hand in both files if that version changes.

## Full context

`RUNBOOK.md` is the authoritative, step-by-step build log — read it before changing the module filters, SDK setup, or flashing config; it documents the actual failures hit while building this (Windows file-lock during `west init`, the `west packages pip --install` trap, the 7-Zip-on-PATH requirement for SDK setup, the pyOCD 0.44.1 bug) and the exact fix for each, plus a troubleshooting table keyed by symptom.
