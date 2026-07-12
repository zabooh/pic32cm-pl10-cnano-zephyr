# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A minimal, from-scratch Zephyr RTOS workspace (`west` topdir) for the **Microchip PIC32CM PL10 Curiosity Nano** board (`pic32cm_pl10_cnano`, SoC `pic32cm6408pl10048`, Arm Cortex-M0+). The application (`app/`) is a blinky plus a small interactive serial command interface — LED control, a stopgap ADC read/stream, and a reset — built on a hand-rolled command parser (`console_getchar()`, not the Zephyr shell subsystem — see Architecture below). `RUNBOOK.md` is the step-by-step log of the *original* lean build (a single `main.c` with `console_getline()`); the app has since been split into modules and grown threads/ADC/history — follow git history and this Architecture section for the current shape, not RUNBOOK's Step 7. `README.md` is the quick-reference summary.

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

There is no test suite in this repo — verification is build success + flash success + manual/`pyserial`-scripted command interaction over the board's virtual COM port at 115200 baud (`led on/off/toggle/blink <ms>`, `adc read`, `adc stream start/stop`, `reset`, `threads`, `mem <addr> [n]`, `help`; a `pl10:~$ ` prompt and boot banner — see Architecture). `mem` is a raw hex dump with **no bounds check** — reading an unmapped/peripheral/misaligned address deliberately HardFaults (a fault-study aid, pairs with the stack-sentinel net). The `threads` command (live per-thread stack high-water marks) is the quickest way to sanity-check stack budgets after touching a thread's call path.

## Architecture

**`app/`** is the only application code. `CMakeLists.txt` points `find_package(Zephyr)` at `$ENV{ZEPHYR_BASE}`; `prj.conf` is the Kconfig (serial console + GPIO + `console_getchar()` + `CONFIG_REBOOT`, `CONFIG_LOG=n`, trimmed kernel stacks — see below). `src/` is split one module per functional domain:

- **`main.c`** — startup wiring only: `led_ctrl_init()` then the default `led_ctrl_blink(500)`, and returns. No hardware details of its own. (Everything else self-starts via `K_THREAD_DEFINE`, so `main()` stays tiny.)
- **`led_ctrl.c` / `led_ctrl.h`** — LED0 (the `led0` devicetree alias → PB2): owns the `gpio_dt_spec` and a dedicated blink thread (`blink_tid`) that toggles the LED on a `k_sleep` interval. `led_ctrl_blink()` sets the rate and `k_wakeup()`s the thread so rate changes / stop take effect immediately instead of at the end of the current sleep. `led_ctrl_on/off/toggle` are the other entry points.
- **`pl10_adc.c` / `pl10_adc.h`** — the ADC, in two layers: a low-level register driver (direct ADC0 register access; see the ADC note below) and an application service on top — `pl10_adc_read_once()` (one-shot read + formatted print) and `pl10_adc_stream_set()` with its `adc_stream_tid` streaming thread (prints a sample every 500 ms until stopped). The parser holds no ADC state; it only calls this interface.
- **`cmd_parser.c`** — the console: its own thread (`cmd_tid`) running a hand-rolled line editor over `console_getchar()` (char-by-char) with echo, end-of-line backspace, and bash-style Up/Down history recall (5-deep ring buffer); dispatches lines via `strcmp`/`strncmp` to the `led`/`adc`/`reset`/`threads`/`help` handlers. Prints the `pl10:~$ ` prompt and, once at startup, a boot banner with the build timestamp (`__DATE__`/`__TIME__`). The `threads` command walks `k_thread_foreach()` and prints each thread's name/priority/live stack usage (needs `CONFIG_THREAD_MONITOR`/`THREAD_NAME`/`THREAD_STACK_INFO`/`INIT_STACKS` — all in `prj.conf`).
- **`app_threads.h`** — not code, just the central budget table: the stack sizes and priorities of all three app threads live here (the `K_THREAD_DEFINE`s stay in their owning modules, so each module stays self-contained, but the tunable numbers are in one place). Sizes are set with headroom over the `threads`-command high-water marks; re-check after changing a thread's call path.

**Five threads total:** `main` + `idle` (kernel baseline) + `blink_tid` (led_ctrl.c, 512 B) + `adc_stream_tid` (pl10_adc.c, 512 B) + `cmd_tid` (cmd_parser.c, 640 B). All three app threads are `K_THREAD_DEFINE` (auto-started at boot), priority 7. **Note:** `main` terminates almost immediately — `main()` only does `led_ctrl_init()` + the default blink and returns — so it won't appear in a `threads` listing.

**Why `console_getchar()` and not `console_getline()`**: the app originally used line-buffered `console_getline()` (see git history / RUNBOOK.md Step 7), but that gives no Up/Down history recall — Zephyr's console driver (`uart_console.c`) silently drops arrow-key escape sequences in line mode. Switched to char-by-char `console_getchar()` with the hand-rolled editor in `cmd_parser.c` to get bash-style history. `CONFIG_CONSOLE_GETCHAR` and `CONFIG_CONSOLE_GETLINE` are a mutually exclusive Kconfig `choice`. **Gotcha:** `printk()` and `console_putchar()` are two independent output paths on the same UART (synchronous `uart_poll_out` vs. the buffered tty layer) — mixing them reorders/drops bytes, so all echo/output in `cmd_parser.c` goes through `printk()` only.

**Why no Zephyr shell**: `west build -t ram_report` showed the shell subsystem alone at **~40-47% of this part's 8 KB RAM** (mostly `CONFIG_SHELL_STACK_SIZE`'s default 2048 B thread stack) for a handful of trivial commands. The hand-rolled parser gives the same UX (prompt, `help`, history) for a fraction of that.

**Kernel's own fixed baseline was trimmed too**: `CONFIG_ISR_STACK_SIZE` 2048→1024, `CONFIG_MAIN_STACK_SIZE` 1024→512 (`prj.conf`). `ISR_STACK_SIZE` is shared by *every* interrupt in the system (GPIO, UART RX, SysTick) plus kernel init — riskier to cut than a single thread's stack, since worst-case need is harder to bound and an overflow is more likely to silently corrupt memory than fault cleanly. Verified stable with an adversarial `pyserial` test (fast blink + rapid concurrent `help`/`adc` commands, the deepest `printk` stack use in the app) — no fault. This is an empirical stress test of *this app's* actual interrupt surface, not a formal worst-case proof; **re-verify with a similar stress test before trusting these values if you add new interrupt sources** (new peripheral driver, higher-priority IRQs).

**Stack-overflow safety net**: `CONFIG_STACK_SENTINEL=y` is enabled (`prj.conf`). This Cortex-M0+ has **no MPU**, so the hardware option (`CONFIG_HW_STACK_PROTECTION`) isn't available (`ARCH_HAS_STACK_PROTECTION` is absent for this SoC); the sentinel is the software fallback — a magic value at each thread stack's bottom, checked at every context switch and on ISRs (`z_check_stack_sentinel`, kernel/thread.c). On overflow it raises a fatal error (`K_ERR_STACK_CHK_FAIL`, reason code 2) naming the thread, instead of silently corrupting memory. Verified by deliberately shrinking `blink_tid`'s stack below its measured need and watching the fatal error fire, then restoring it. It's a *reactive* net (detected at the next checkpoint, after the overflow); `INIT_STACKS` + the `threads` command are the *proactive* side (measure headroom before it bites).

**The ADC is a stopgap direct-register driver, not a Zephyr driver.** Mainline Zephyr (and Microchip's downstream fork) have no `adc_driver_api`/devicetree binding for the PIC32CM PL family's ADC — its register map matches neither in-tree Microchip ADC driver (`adc_mchp_g1`, `adc_sam0`). So `pl10_adc.c` pokes ADC0 registers directly, using the register definitions from the `hal_microchip` pack header (`pic32cm6408pl10048.h`, same header the CMSIS world uses). Clock (`MCLK.APBCMASK` bit 7, no GCLK channel needed) and pin (**AIN29 / PA29**, the one ADC-capable pin this board breaks out) were cross-verified against three independent sources: the datasheet (DS40002667A §11.6.8, §33.4.2.5), the CMSIS reference in `C:\work\Bukarest\3_Mi_CMSIS\`, and Microchip's official Harmony reference app for this exact board (`csp_apps_pic32cm_pl10` v1.0.0). Clock/pinctrl go through direct register writes rather than Zephyr's `clock_control`/`pinctrl` because there is no Zephyr ADC node to hang them off — if a real Zephyr PL10 ADC driver lands upstream later, replace this module with it. On the bare Curiosity Nano, AIN29 floats, so `adc read` returns noisy values near VDD — expected, not a bug.

Current usage: **FLASH 30.14% (18516/61440 B), RAM 61.04% (5000/8192 B)** — re-check with `ram_report`/`rom_report` (or the `threads` command) after any change, this part has limited headroom to spare. Most of the RAM growth from the original 33.50% baseline is the three app threads (per-thread cost = its stack of 512/512/640 B + a 120 B thread control block) plus the thread-introspection config (`THREAD_NAME`/`STACK_INFO`/`MONITOR`, ~290 B). Budget for a handful more small threads before RAM becomes the constraint.

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
