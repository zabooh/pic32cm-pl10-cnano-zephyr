# Runbook: Lean Zephyr Installation for PIC32CM PL10 Curiosity Nano (Windows, C:\zw)

**Location of this file:** `C:\zw\RUNBOOK.md`
**Executor:** Claude Code agent on Windows (native, no WSL2)

## Task (summary)

Set up a **as lean as possible** Zephyr installation under `C:\zw` that contains only what's needed for the **PIC32CM PL10 Curiosity Nano** board. Build a **blinky program with a serial command interface** that lets you control the LED and read the ADC in different ways (originally the Zephyr shell subsystem; replaced with a hand-rolled `console_getchar()` parser plus a link-time command registry once RAM profiling showed the shell alone using ~40-47% of this part's 8 KB RAM — see Step 7). Flash it. At the end, produce a **pinned reproduction script** that deterministically recreates this exact installation anywhere.

Guiding principle throughout: **leanness**. Only the ARM toolchain, only the modules PIC32CM actually needs, shallow clone.

> **Update after the first successful run (2026-07-11):** This runbook was revised after a full end-to-end run (build + flash + shell test all succeeded). The most important corrections versus the original version:
> 1. **`west group-filter` alone is not enough for "lean"** — it only excludes whole manifest groups (e.g. `hal`), not individual vendor HALs. Without an additional `manifest.project-filter`, `west update` pulls in dozens of irrelevant modules (mbedtls, lvgl, trusted-firmware-a/m, hostap, nrf_wifi, …) totaling several GB — see Step 3.
> 2. **`west packages pip --install` installs Zephyr's entire requirements set** (including test/compliance packages) and can fail on Windows on `hidapi`, which demands missing MSVC build tools. Instead, install `requirements-base.txt` specifically — see Step 5.
> 3. **`pyOCD 0.44.1` is broken for this board** (SWD "FAULT ACK"/"No ACK" in every connect mode). Always pin **`pyocd==0.43.0`** — see Step 8 and Troubleshooting.
> 4. The SDK setup (`west sdk install`) **requires `7z` literally on PATH** (not just 7-Zip installed), otherwise toolchain registration aborts and the actual GNU toolchain is never downloaded — see Step 5 and Troubleshooting.
> 5. `west init` can fail on Windows with a transient `PermissionError` (likely antivirus scanning) while moving/cleaning up the freshly cloned repo, even though the clone itself is complete — see Troubleshooting.

---

## Constraints (agent, please observe)

- **Keep the working directory short:** everything under `C:\zw`. Reason: Zephyr limits object file paths to 250 characters (`CMAKE_OBJECT_PATH_MAX`), and Windows `MAX_PATH` makes this worse. A short path preemptively prevents the most common Windows build errors.
- **Ninja must be on PATH**, so CMake doesn't fall back to "NMake Makefiles" (a known Windows failure mode without Visual Studio build tools).
- **One change per iteration.** If a build step fails, consult the "Troubleshooting" section (below), apply **exactly one** fix, and rebuild with `-p always` (pristine).
- **Verify two values yourself, don't guess:** the exact board target name and the pyOCD target name for the PL10 (see Steps 6 and 8). If your verification differs from the name assumed here, use the verified value.
- **Avoid interactive questions** where a value can be determined on its own. If a value can't be found, stop and report precisely what's missing.

---

## Checking prerequisites

Check whether the following tools are on PATH. If one is missing, stop and report it with an installation hint (don't install anything large without asking):

- `python --version` (3.12 recommended; newer versions generally work too, see Troubleshooting)
- `git --version`
- `ninja --version`  ← critical (see above)
- `cmake --version`
- `7z --version` or check for `C:\Program Files\7-Zip\7z.exe`  ← needed internally by `west sdk install` for toolchain registration (not just "installed" but **on PATH**). If present but not on PATH: add it for the current session: `$env:PATH = 'C:\Program Files\7-Zip;' + $env:PATH`. If 7-Zip is missing entirely, stop and report it (don't install it without asking).

Optional, against slow builds: exclude the `C:\zw` folder from Windows Defender real-time scanning (only if considered trustworthy):
```powershell
Add-MpPreference -ExclusionPath 'C:\zw'
```

---

## Step 1 — Workspace scaffold

```powershell
New-Item -ItemType Directory -Force -Path C:\zw | Out-Null
Set-Location C:\zw
python -m venv C:\zw\.venv
& C:\zw\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install west
```

> Remember for later sessions: in every new PowerShell, first run `& C:\zw\.venv\Scripts\Activate.ps1`.

---

## Step 2 — Initialize the west workspace

```powershell
west init -m https://github.com/zephyrproject-rtos/zephyr C:\zw
Set-Location C:\zw
```

If `.west` already exists: the workspace is already initialized, proceed to Step 3.

> **Known Windows pitfall:** The clone step itself (file checkout) can complete fully and `west init` can still report a `PermissionError`/`WinError 5` at the end while moving or deleting the temporary clone directory (`.west\manifest-tmp`) — typically because a real-time antivirus scanner briefly locks a file under `.git\objects\pack\*.idx`. **Before restarting, check whether the clone is actually damaged:** `git -C C:\zw\zephyr status` and `git -C C:\zw\zephyr log -1`. If the repo is clean and up to date but `.west\config` is missing (`Test-Path C:\zw\.west\config`), it's enough to add the configuration manually instead of re-cloning:
> ```powershell
> @"
> [manifest]
> path = zephyr
> file = west.yml
> "@ | Set-Content C:\zw\.west\config
> Remove-Item -Recurse -Force C:\zw\.west\manifest-tmp -ErrorAction SilentlyContinue   # retry if needed; harmless if still locked the first time
> west list   # should now show the project list without error
> ```

---

## Step 3 — Lean module set (the central leanness step)

Goal: only the modules PIC32CM actually needs. **`hal_microchip`** is required for PIC32CM (it contains the PIC32C/PIC32CM DFP headers). Whether an Atmel/SAM dependency is actually needed depends on the specific SoC family — **don't assume it, verify it in the cloned `zephyr` repo** (see point 2 below). Also firmly required is **CMSIS** (`cmsis`, `cmsis_6`) — Zephyr generically selects `HAS_CMSIS_CORE` for every Cortex-M core (`zephyr/arch/arm/core/Kconfig`), independent of vendor.

> **Key lesson learned in practice:** `west config manifest.group-filter` is **coarse-grained** — it can only toggle whole manifest groups on/off (e.g. `hal`, `optional`, `babblesim`, `ci`). All vendor HALs (`hal_microchip`, `hal_nordic`, `hal_st`, `hal_nxp`, …) share the same `hal` group — excluding `hal` removes `hal_microchip` along with it, including it pulls in **all** other vendor HALs too. Likewise, plenty of "mandatory" modules (not marked `optional`) from the `fs`, `crypto`, `tee`, `debug`, `tools`, `lib` groups stay active **regardless** of how the group filter is set — among them `mbedtls`, `trusted-firmware-a`/`-m`, `lvgl`, `hostap`, `nrf_wifi`, `openthread`, `acpica`. With `group-filter` alone, `west update` pulls in dozens of irrelevant modules totaling several GB.
>
> **For real leanness, also set `manifest.project-filter`** — a regex allow/deny list on full project names (documented in the docstring of `west/manifest.py`, which even gives the example `-hal_.*,+hal_my_vendor`). This lets you keep precisely what's needed active, independent of group membership.

1. View available groups (the `west list-groups` command does **not** exist in current west versions — check the manifest directly instead):
   ```powershell
   Select-String -Path C:\zw\zephyr\west.yml -Pattern "group-filter:|^\s*groups:" -Context 0,1
   ```
2. **Verify HAL dependencies in the source instead of guessing** (example for PL10; the pattern applies to any SoC family): which device header does `soc.h` include, and which vendor directory does it come from?
   ```powershell
   Select-String -Path "C:\zw\zephyr\soc\microchip\pic32c\pic32cm_pl\pic32cm_pl10\soc.h" -Pattern "#include"
   Select-String -Path "C:\zw\zephyr\boards\microchip\pic32c\pic32cm_pl10_cnano\pic32cm_pl10_cnano.dts" -Pattern "atmel|microchip" -CaseSensitive:$false
   ```
   If no `atmel,...` compatible strings show up (as with the PL10 family), `hal_atmel` is **not** needed — unlike older PIC32CM-SG boards, which reuse SAM IP. When in doubt, build first without `hal_atmel` (Step 8) and only add it in response to a concrete "missing header" error (one change per iteration).
3. Set the group filter (a starting point that excludes coarse optional groups; **always quote the value in PowerShell**, otherwise the commas/dashes cause a parser error):
   ```powershell
   west config manifest.group-filter -- "-optional,-babblesim,-ci"
   ```
4. Set the project filter as an allow-list — only actually-needed projects active, everything else (even from mandatory groups) inactive:
   ```powershell
   west config manifest.project-filter -- "-.*,+hal_microchip,+cmsis,+cmsis_6,+picolibc"
   ```
   `picolibc` is the default C library in current Zephyr (`LIBC_IMPLEMENTATION` defaults to `PICOLIBC`, see `zephyr/lib/libc/Kconfig`) and should be included.
5. Double-check both filter values with `west config -l | Select-String manifest` and note them down for the reproduction script (Step 10).

---

## Step 4 — Fetch modules shallow

```powershell
west update --narrow --fetch-opt=--depth=1
```

Afterward, verify that `hal_microchip` is present:
```powershell
west list | Select-String hal_microchip
```
If it's missing: the group/project filter was too strict — loosen it and run `west update` again.

**Cross-check the scope** (was the project filter from Step 3 actually applied, or did `west update` already run earlier with a too-coarse filter?):
```powershell
west list   # should show ONLY the projects allowed by the project-filter + the zephyr manifest
Get-ChildItem C:\zw\modules -Recurse -Directory -Filter ".git" | Measure-Object   # rough module count
```
If significantly more than the expected 4–5 projects show up (e.g. because `west update` ran before the `project-filter` was set): **don't** hard-kill the running `west update` process (identifying processes only by name/start time is risky and can hit unrelated processes) — instead let it finish, then set the filter from Step 3 and manually delete the module folders under `C:\zw\modules` (and possibly `C:\zw\bootloader`, `C:\zw\tools`) that are no longer active, to get back to a lean state. Size comparison before/after: `Get-ChildItem C:\zw\modules -Recurse | Measure-Object -Property Length -Sum`.

---

## Step 5 — Export and lean toolchain

```powershell
$env:CMAKE_GENERATOR = "Ninja"     # prevents NMake fallback
west zephyr-export
```

> **Do not use `west packages pip --install`** (nor its fallback `pip install -r requirements.txt`) — both install Zephyr's **entire** requirements set (base + build-test + run-test + extras + compliance), including packages irrelevant to us like `opencv-python`, `spsdk`, `pylint`, `mypy`, `tree-sitter`. On Windows this can fail on `hidapi` (`error: Microsoft Visual C++ 14.0 or greater is required`), because an older `hidapi` version forced by the test packages' version constraints has no prebuilt wheel for the installed Python version. Instead, install **only the base actually needed to build**:
> ```powershell
> pip install -r C:\zw\zephyr\scripts\requirements-base.txt
> ```
> pyOCD is installed separately in Step 8 (with a pinned version — see there, also to avoid this exact `hidapi` problem: `pip install pyocd` without the test-package constraints picks a newer `hidapi` version with a matching wheel).

SDK **with the ARM toolchain only** (no full bundle), optionally also without extra host tools (OpenOCD/QEMU aren't needed since flashing goes through pyOCD):
```powershell
Set-Location C:\zw\zephyr
west sdk list
west sdk install -t arm-zephyr-eabi -H
```

> **Known Windows pitfall:** The SDK's own `setup.cmd` (registers the CMake toolchain, runs automatically at the end of `west sdk install`) aborts with `Zephyr SDK setup requires '7z' to be installed and available in the PATH` if 7-Zip is installed but not **literally as `7z`** on PATH (Patool/west itself find 7-Zip via its full path for extraction — but the SDK's own setup script doesn't). Symptom: download and extraction complete cleanly, but `sdk_gnu_toolchains\` stays **empty**, because the actual GNU toolchain is only fetched as the last step after setup succeeds. Fix (one change, then repeat the step):
> ```powershell
> $env:PATH = 'C:\Program Files\7-Zip;' + $env:PATH
> west sdk install -t arm-zephyr-eabi -H   # run again; detects the existing download, fetches only the missing toolchain
> ```
> Success check: `west sdk list` should show `arm-zephyr-eabi` under `gnu-installed-toolchains:`, and `Get-ChildItem <SDK path>\gnu\arm-zephyr-eabi` should not be empty.

---

## Step 6 — Verify the board target

Determine the exact board name for the PL10 Curiosity Nano:
```powershell
west boards | Select-String -Pattern pl10 -CaseSensitive:$false
```
Expect a name like `pic32cm_pl10_curiosity_nano` (or similar). **Use the name actually listed** in the following steps. If no PL10 board is found, stop and report: the Zephyr revision in use contains no PL10 target — then check the board support level.

Set the verified name as the placeholder `<BOARD>` for the rest of the runbook.

---

## Step 7 — Create the application (modular, with a command registry)

Create a **standalone, minimal application** under `C:\zw\app` (don't copy the Zephyr sample — leaner and self-contained). It is split **one module per functional domain**, and the console is **feature-agnostic**: each module *self-registers* its command into a flash-resident registry, so adding a command never touches the parser. This section is generic on purpose — the pattern (module-per-domain + link-time command registry) reproduces on any Zephyr board; only the board name, the peripheral driver, and the pinned config values are specific.

> **How it got to this shape (historical).** It began as a single `main.c` using `CONFIG_SHELL`; on this 8 KB-RAM part `ram_report` put the shell subsystem alone at ~47% of RAM for four trivial commands. It was replaced by a hand-rolled parser over `console_getline()` (−~15 KB flash / −~3.7 KB RAM for the same UX), then switched to `console_getchar()` to gain bash-style Up/Down history (line mode drops arrow escapes), and finally the hard-coded `strcmp` dispatch was inverted into the command registry below so features stopped being welded to the parser. The no-shell and stack-trimming rationale in the notes below still explains the `prj.conf` choices.

**Directory layout** (one module per domain; each header sits next to its source):
```
C:\zw\app\
├── CMakeLists.txt
├── cmd_sections.ld          # linker fragment declaring the "cmd" iterable ROM section
├── prj.conf
└── src\
    ├── main.c               # startup wiring only (peripheral init + default action)
    ├── led_ctrl.c/.h        # LED0 + blink thread + "led" command
    ├── pl10_adc.c/.h        # ADC0 register driver + stream thread + "adc" command
    ├── cmd_parser.c         # console infrastructure (line editor, history, dispatch) + "help"
    ├── cmd.h                # command-registry interface + CMD_REGISTER macro
    ├── diag.c               # "reset"/"threads"/"mem" system commands
    ├── fault.c              # k_sys_fatal_error_handler override (names the faulting thread)
    └── app_threads.h        # central thread stack/priority budgets
```

**`C:\zw\app\CMakeLists.txt`:**
```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pic32cm_pl10_blinky_shell)
target_sources(app PRIVATE
    src/main.c src/led_ctrl.c src/pl10_adc.c src/cmd_parser.c src/fault.c src/diag.c)

# Declare the "cmd" console-command registry as an iterable ROM section (see
# cmd_sections.ld + cmd.h). SECTIONS (not ROM_SECTIONS): ITERABLE_SECTION_ROM
# already places itself into the ROMABLE_REGION.
zephyr_linker_sources(SECTIONS cmd_sections.ld)
```

**`C:\zw\app\prj.conf`:**
```
CONFIG_GPIO=y
CONFIG_SERIAL=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_LOG=n
CONFIG_REBOOT=y

# Print the boot banner ourselves (with a leading newline, so it survives a
# fault-reboot's non-newline-terminated dump) - disable Zephyr's own.
CONFIG_BOOT_BANNER=n

# Char-by-char console (not the shell subsystem, not line mode) so the
# hand-rolled editor can implement Up/Down history - line mode drops the arrow
# escape sequences. GETCHAR and GETLINE are a mutually exclusive Kconfig choice.
CONFIG_CONSOLE_SUBSYS=y
CONFIG_CONSOLE_GETCHAR=y

# Kernel's fixed baseline stacks trimmed from their defaults (ISR_STACK_SIZE 2048,
# MAIN_STACK_SIZE 1024) - see the second note below for the risk and how this was
# verified.
CONFIG_ISR_STACK_SIZE=1024
CONFIG_MAIN_STACK_SIZE=512

# Thread introspection for the "threads" command (live per-thread stack usage).
CONFIG_THREAD_MONITOR=y
CONFIG_THREAD_NAME=y
CONFIG_THREAD_MAX_NAME_LEN=16
CONFIG_THREAD_STACK_INFO=y
CONFIG_INIT_STACKS=y

# Software stack-overflow detection (this Cortex-M0+ has no MPU, so the hardware
# option CONFIG_HW_STACK_PROTECTION is unavailable).
CONFIG_STACK_SENTINEL=y

# On a fatal error, print the dump and reboot instead of halting forever.
CONFIG_RESET_ON_FATAL_ERROR=y
```

> **This app originally used `CONFIG_SHELL` (full Zephyr shell subsystem: `on`/`off`/`toggle`/`blink` as shell subcommands with a `pl10:~$` prompt, tab completion, `help`).** On this 8 KB-RAM part that turned out to be a poor fit: `west build -d C:\zw\build -t ram_report` showed the shell subsystem alone at **47% of total RAM** — a 2048-byte thread stack plus a 512-byte history heap, for four trivial GPIO commands. Even after trimming (`CONFIG_SHELL_STACK_SIZE=1536`, `CONFIG_SHELL_HISTORY=n`, `CONFIG_SHELL_GETOPT=n` — chosen after `1024` caused a **hard fault** running `help`, which walks the whole command tree and needs more stack depth than any single app command) RAM usage was still 83.50%, next to no headroom for real application logic.
>
> **Replaced with `console_getline()`** — Zephyr's built-in lightweight pull-style line reader (`zephyr/console/console.h`), which runs synchronously in the calling thread with no shell thread/stack/history buffers of its own. Combined with a `k_timer` to decouple LED blinking from command reading (see `main.c` below), this dropped usage to **FLASH 26.71% (16408 B, was 66.56%) and RAM 52.25% (4280 B, was 83.50%)** — verified stable and functionally identical via the same `pyserial` test as before (all `led` subcommands plus an unknown-command case). A hand-written `pl10:~$ ` prompt and `help` command were added back afterward for UX parity with the old shell (see `main.c`) — cost was **only +260 B flash, +0 B RAM** (static string literals, no new buffers), vs. the shell's ~15 KB flash / ~3.7 KB RAM for the same features. (These figures and the `k_timer`/`main.c` details describe the `console_getline()` stage; the app has since moved to `console_getchar()` with Up/Down history, per-domain threads, and the link-time command registry described below — so command parsing is no longer a `strcmp` chain in `main.c`. The shell-vs-hand-rolled RAM rationale is what still stands.)
>
> **Kernel's fixed baseline stacks trimmed too — `CONFIG_ISR_STACK_SIZE` (default 2048) and `CONFIG_MAIN_STACK_SIZE` (default 1024).** Even after the shell was gone, `ram_report` showed `kernel` at 88% of the (now much smaller) RAM total — almost entirely `z_interrupt_stacks` (2048 B) + `z_main_stack` (1024 B), fixed costs independent of what the app does. **This is riskier to cut than the shell's per-thread stack was**: `ISR_STACK_SIZE` is shared by *every* interrupt in the system (GPIO, UART RX, SysTick/timer) plus kernel init, so its worst-case need is harder to bound than one thread's, and an overflow here is more likely to silently corrupt adjacent memory than to produce a clean fault. Tested at `ISR_STACK_SIZE=1024` / `MAIN_STACK_SIZE=512` (halved) with a deliberately adversarial `pyserial` script: `led blink 10` (10 ms timer-ISR period — the fastest realistic interrupt load in this app) running concurrently with 15 rapid-fire `help` calls (the single deepest `printk` call/format-string stack use in the app) over UART — no fault, no corrupted output, across the whole run. Result: **RAM 52.25%→33.50% (4280 B→2744 B)**. This is an empirical stress test covering this app's actual interrupt surface (GPIO/UART/timer only) under realistic-to-heavy load, **not a formal worst-case stack proof** — if you add new interrupt sources (a new peripheral driver, nested/higher-priority IRQs) later, re-verify with a similar stress test rather than assuming headroom carries over.

**Modules & threads.** `main.c` does startup wiring only — initialize the LED and start the default blink, then return; every other module self-starts via `K_THREAD_DEFINE`. Blinking and ADC streaming each run in their own thread using an interruptible `k_sleep` (woken with `k_wakeup` so a rate change/stop takes effect immediately), rather than a `k_timer` firing in ISR context. Stack sizes and priorities for all app threads are centralized in `app_threads.h`, set with headroom over the live high-water marks the `threads` command reports.

**The command registry (the reusable pattern).** The parser knows nothing about `led`/`adc`/etc. Each command is a `const struct cmd` placed at link time into a custom iterable ROM section named `cmd` — the same mechanism Zephyr's `SHELL_CMD_REGISTER` uses, minus the shell's RAM cost (the table lives in flash, ~0 B RAM). Three pieces:

`src/cmd.h` — interface + registration macro:
```c
#include <zephyr/sys/iterable_sections.h>
typedef void (*cmd_fn_t)(int argc, char **argv);
struct cmd { const char *name; cmd_fn_t fn; const char *help; };
#define CMD_REGISTER(id, name_, fn_, help_) \
    static const STRUCT_SECTION_ITERABLE(cmd, _cmd_##id) = { \
        .name = (name_), .fn = (fn_), .help = (help_) }
```

`cmd_sections.ld` — declares the section (emits the `_cmd_list_start`/`_cmd_list_end` bounds that `STRUCT_SECTION_FOREACH` walks):
```c
#include <zephyr/linker/iterable_sections.h>
ITERABLE_SECTION_ROM(cmd, Z_LINK_ITERABLE_SUBALIGN)
```

A feature module registers its command and owns its own subcommand parsing:
```c
static void led_cmd(int argc, char **argv) { /* parse on/off/toggle/blink <ms> */ }
CMD_REGISTER(led, "led", led_cmd, "led on|off|toggle|blink <ms>  - LED control");
```

`cmd_parser.c` is generic infrastructure: a `console_getchar()` line editor (with 5-deep Up/Down history), a tokenizer splitting the line into `argc`/`argv`, and dispatch:
```c
STRUCT_SECTION_FOREACH(cmd, c)
    if (strcmp(argv[0], c->name) == 0) { c->fn(argc, argv); return; }
printk("unknown command: %s\n", argv[0]);
```
The built-in `help` iterates the same registry, so there is no second, hand-maintained command list. Adding a command is one `CMD_REGISTER` in its owning module; the parser is never touched.

**Registry gotchas:** entries must be `const` (else they land in RAM, not the ROM section); use `zephyr_linker_sources(SECTIONS ...)` not `ROM_SECTIONS` (the `ITERABLE_SECTION_ROM` macro self-places into ROM); the `KEEP()` inside that macro stops the linker garbage-collecting the never-directly-referenced entries; and `help` order equals link order, not alphabetical.

**Fault attribution (`fault.c`).** Because `CONFIG_LOG=n` compiles out the kernel's own `>>> ZEPHYR FATAL ERROR N` / `Current thread:` lines (they go through `LOG_ERR`), a raw fault/stack-overflow dump on the console can't be tied to a thread. `src/fault.c` overrides the `__weak k_sys_fatal_error_handler()` to `printk` the reason code + `k_thread_name_get(k_current_get())` before rebooting; reboot-vs-halt still follows `CONFIG_RESET_ON_FATAL_ERROR`.

For the full implementation of each module, read the source files — they are self-contained and commented; this runbook documents the *structure and reproducible recipe*, not a line-by-line mirror.

> If the build reports that `led0` is missing: the alias may be named differently on this board. Check the board DTS
> (`C:\zw\zephyr\boards\...\*pl10*\*.dts`) for the LED alias/label and adjust the `DT_ALIAS(led0)` node in `led_ctrl.c`.
>
> `CONFIG_CONSOLE_GETCHAR` needs `CONFIG_CONSOLE_SUBSYS` and the UART driver's interrupt-driven RX (satisfied by `CONFIG_UART_CONSOLE=y` + the SERCOM UART driver); call `console_init()` once before first use. `CONSOLE_GETCHAR`, `CONSOLE_GETLINE`, and the shell subsystem are mutually exclusive — enable exactly one console input mode. **Gotcha:** `printk()` and `console_putchar()` are independent output paths on the same UART; mixing them reorders/drops bytes, so all echo/output goes through `printk()` only.

---

## Step 8 — Build and flash

**Build** (keep the build directory short — path length):
```powershell
Set-Location C:\zw\zephyr
west build -p always -b <BOARD> -d C:\zw\build C:\zw\app
```
Success → artifact at `C:\zw\build\zephyr\zephyr.hex`.

**Verify the pyOCD target and flash:**

> ⚠️ **`pyOCD 0.44.1` is broken for the PIC32CM PL10** — flash/connect attempts fail with `SWD/JTAG communication failure (FAULT ACK)` or `(No ACK)` already at debug port setup, regardless of connect mode (`halt`, `under-reset`, `pre-reset`) or SWD frequency. Confirmed twice independently on the same machine (once in a CMSIS/MPLAB toolchain, once in this Zephyr setup). **Always pin `pyocd==0.43.0`:**
> ```powershell
> pip install "pyocd==0.43.0"
> ```

```powershell
pyocd list --targets | Select-String -Pattern pic32cm -CaseSensitive:$false
```
Use the listed PIC32CM target name as `<PYOCD_TARGET>` (a Microchip DFP pack may be required; if the PL10 doesn't appear, install the matching pack via `pyocd pack`). Alternatively, just look at the board's runner definition — it directly contains both the target **and** the reduced SWD frequency this board needs:
```powershell
Get-Content C:\zw\zephyr\boards\microchip\pic32c\pic32cm_pl10_cnano\board.cmake
```

**Prefer `west flash` over manual `pyocd flash`** — it automatically applies exactly the runner arguments defined in `board.cmake` (target **and** frequency), instead of having to guess/copy the frequency by hand:
```powershell
Set-Location C:\zw\build
west flash
```
Equivalent manually (if `<PYOCD_TARGET>` and `<PYOCD_FREQUENCY>` are already known from `board.cmake`):
```powershell
pyocd flash -t <PYOCD_TARGET> -f <PYOCD_FREQUENCY> C:\zw\build\zephyr\zephyr.hex
```

**If an SWD error occurs despite the pinned pyOCD 0.43.0, but a different program was previously running on the board:** briefly unplugging/replugging the board and retrying the flash is usually enough — a single failed connect attempt can already have halted the core via reset without the session being cleanly established. Only assume a physical/cable problem after that (see Troubleshooting).

---

## Step 9 — Functional test via the serial console

Connect a Windows terminal (e.g. PuTTY/TeraTerm) to the board's COM port (115200 baud). The prompt `pl10:~$ ` appears (the console is the hand-rolled `console_getchar()` parser + command registry from Step 7, with Up/Down history). Type a command and press Enter; it's echoed back, then the response is printed, then the prompt again:

```
pl10:~$ led on
pl10:~$ led blink 100
pl10:~$ adc read
pl10:~$ adc stream start
pl10:~$ adc stream stop
pl10:~$ threads
pl10:~$ mem 0x20000000 64
pl10:~$ help
```

Success criteria: the LED responds as expected (steady on/off, toggling, different blink rates); `adc read`/`stream` print samples; `threads` lists each thread's live stack usage; `mem` hex-dumps the address; `help` prints the full command list (assembled from the registry — every registered command appears); Up/Down recalls history; and an unrecognized line prints `unknown command: <line>` rather than silently doing nothing.

> **Automatable for the agent:** Instead of a manual terminal, the functional test can be automated with `pyserial` (comes along via `requirements-base.txt`) — find the COM port via `Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -match "Curiosity Virtual COM|EDBG Virtual COM" }`, then connect with `serial.Serial(<PORT>, 115200)`, send commands (`\r\n`-terminated), and check the responses against the expected strings (`led on`, `led toggled`, `led blink <ms> ms`, …). This fully replaces the manual PuTTY/TeraTerm step and provides a solid success criterion without user interaction.

---

## Step 10 — Produce the pinned reproduction script

**Only run this once Steps 8 and 9 have succeeded** (otherwise the harvested values aren't trustworthy).

### 10a — Harvest the actual values of the working installation

```powershell
Set-Location C:\zw
$ZEPHYR_REV     = (git -C C:\zw\zephyr rev-parse HEAD).Trim()
$WEST_VER       = (pip show west   | Select-String '^Version:' ).ToString().Split(' ')[-1]
$PYOCD_VER      = (pip show pyocd  | Select-String '^Version:' ).ToString().Split(' ')[-1]
$PY_VER         = (python -c "import sys;print('.'.join(map(str,sys.version_info[:3])))").Trim()
$SDK_LIST       = (west sdk list | Out-String)
$GROUPFILTER    = (west config manifest.group-filter)
$PROJECTFILTER  = (west config manifest.project-filter)
pip freeze | Out-File -Encoding ascii C:\zw\requirements-lock.txt
```

Determine the installed **SDK version** from `$SDK_LIST` (the version number of the `arm-zephyr-eabi` toolchain) and keep it as `$SDK_VER`.

Also have the verified names from Steps 6/8 ready: `<BOARD>`, `<PYOCD_TARGET>`, and `<PYOCD_FREQUENCY>` (`board.cmake`'s value is a safe starting point, 100000 for this board — but this workspace's own debugging config raises it to `2000000` after verifying it stable, see Step 11c; use whichever you've actually verified).

> Explicitly check `$PYOCD_VER` against the known pitfall: if it's `0.44.1`, Step 8 wasn't run with the pinned version — fix it before harvesting (`pip install "pyocd==0.43.0"`), otherwise the broken version ends up in the reproduction script.

### 10b — Write `C:\zw\reproduce-install.ps1`

Produce a **non-interactive, version-pinned** file with exactly these fixed values. It must run without any prompts and produce the same installation. Structure:

```powershell
# reproduce-install.ps1  — pinned reproduction for PIC32CM PL10 Curiosity Nano
# Generated automatically from a verified installation.
#
# Portable by design: builds the workspace in whatever directory this script itself
# lives in (via $PSScriptRoot), NOT a hardcoded path. To reproduce this setup, copy
# this script plus requirements-lock.txt and the app\ folder into an empty target
# directory, then run it from there.
$ErrorActionPreference = "Stop"
if (-not $PSScriptRoot) {
  throw "This script must be run as a file (e.g. 'powershell -File reproduce-install.ps1'), not pasted into a console - `$PSScriptRoot is only set when running from a saved .ps1 file."
}
$WS = $PSScriptRoot

# $ErrorActionPreference = "Stop" only catches PowerShell-native errors - a failing
# external command (west, git, pip, pyocd) just sets $LASTEXITCODE and lets the script
# keep going, cascading into confusing unrelated errors further down. Wrap every external
# call in this function so a real failure stops the script immediately, at its source.
function Invoke-Checked {
  param([Parameter(Mandatory)][ScriptBlock]$Command)
  & $Command
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with exit code $($LASTEXITCODE): $Command"
  }
}

# --- pinned values (filled in by the agent) ---
$ZEPHYR_REV      = "<harvested commit hash>"
$WEST_VER        = "<harvested>"
$SDK_VER         = "<harvested>"
$BOARD           = "<BOARD>"
$PYOCD_TARGET    = "<PYOCD_TARGET>"
$PYOCD_VER       = "0.43.0"   # IMPORTANT: 0.44.1 is broken for this board (SWD FAULT ACK/No ACK) - do not
                              # change without re-verifying the fix.
$GROUP_FILTER    = "<pinned group filter>"
$PROJECT_FILTER  = "<pinned project filter>"     # e.g. "-.*,+hal_microchip,+cmsis,+cmsis_6,+picolibc"
$PYOCD_FREQUENCY = "<pinned SWD frequency, verified stable for this board>"

# --- prerequisites ---
foreach ($t in 'python','git','ninja','cmake') {
  if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t is missing from PATH" }
}

# 7-Zip is required by the Zephyr SDK setup strictly via PATH (not just installed).
# Without it, 'west sdk install' aborts while registering the CMake package and the
# actual GNU toolchain is never downloaded.
$sevenZip = 'C:\Program Files\7-Zip'
if ((Test-Path "$sevenZip\7z.exe") -and ($env:PATH -notlike "*$sevenZip*")) {
  $env:PATH = "$sevenZip;$env:PATH"
} elseif (-not (Get-Command 7z -ErrorAction SilentlyContinue)) {
  throw "7-Zip (7z.exe) is missing from PATH - required by the Zephyr SDK setup. Please install 7-Zip."
}

# --- venv + pinned tools ---
New-Item -ItemType Directory -Force -Path $WS | Out-Null
Invoke-Checked { python -m venv $WS\.venv }
& $WS\.venv\Scripts\Activate.ps1
Invoke-Checked { python -m pip install --upgrade pip }
Invoke-Checked { pip install "west==$WEST_VER" }

# --- workspace, then pin the manifest repo (zephyr/) to the exact commit ---
# 'west init -m <url> --mr <SHA>' does NOT work: west runs 'git clone --branch <mr>',
# and git's --branch only resolves ref NAMES (branches/tags) - never a raw commit SHA,
# even when that SHA happens to be a branch's current tip (verified empirically against
# a throwaway test repo, see Step 10d). The correct pattern is to clone the default
# branch (a full, non-shallow clone - west's init never passes --depth for the manifest
# repo, so the complete history is already there) and then check out the pinned commit
# explicitly.
Invoke-Checked { west init -m https://github.com/zephyrproject-rtos/zephyr $WS }
Set-Location $WS\zephyr
Invoke-Checked { git checkout $ZEPHYR_REV }
Set-Location $WS
Invoke-Checked { west config manifest.group-filter -- "$GROUP_FILTER" }
Invoke-Checked { west config manifest.project-filter -- "$PROJECT_FILTER" }
Invoke-Checked { west update --narrow --fetch-opt=--depth=1 }

# --- export + pinned Python deps ---
# Deliberately ONLY requirements-lock.txt (not 'west packages pip --install' /
# the full requirements.txt): the latter also pulls in test/compliance packages
# (e.g. opencv, spsdk), of which 'hidapi' in certain version combinations forces
# an MSVC build tools build on Windows, which we deliberately avoid.
# requirements-lock.txt already contains the pinned pyocd==0.43.0.
$env:CMAKE_GENERATOR = "Ninja"
Invoke-Checked { west zephyr-export }
Invoke-Checked { pip install -r $WS\requirements-lock.txt }

# --- SDK pinned, ARM only, without extra host tools ---
Set-Location $WS\zephyr
Invoke-Checked { west sdk install -t arm-zephyr-eabi -H --version $SDK_VER }

# --- the application ships alongside this script ($WS\app) ---
# Verification build + flash:
Invoke-Checked { west build -p always -b $BOARD -d $WS\build $WS\app }
Invoke-Checked { pyocd flash -t $PYOCD_TARGET -f $PYOCD_FREQUENCY $WS\build\zephyr\zephyr.hex }
Write-Host "Reproduction complete." -ForegroundColor Green
```

> The agent fills in the `<...>` placeholders with the harvested values. The app files
> (`app\*`) and `requirements-lock.txt` are part of the reproduction and must sit next to
> `reproduce-install.ps1` in the same directory before running it — the script does **not**
> fetch or copy them itself, it only reads them from `$PSScriptRoot`.
> `requirements-lock.txt` comes from `pip freeze` **after** the pyOCD-0.43.0 pin in Step 8 — that way the correct version is already in the lock file and doesn't need to be installed separately in the script.
> Because `$WS` is derived from the script's own location rather than hardcoded, the same package can be reproduced at any target path — just create an (empty) directory, copy `reproduce-install.ps1` + `requirements-lock.txt` + `app\` into it, and run the script from there (see README.md "Reproducing this setup elsewhere").

### 10d — Known bug found and fixed: `west init --mr <SHA>` doesn't work

An early version of this script pinned the manifest repo via `west init -m <url> --mr $ZEPHYR_REV $WS`, on the (wrong) assumption that `--mr` accepts a branch, tag, *or* commit SHA equally. It doesn't for the initial clone: west runs `git clone --branch <mr> ...` under the hood, and git's `--branch` flag only resolves **ref names** (branches/tags) via `ls-remote` — it never accepts a raw commit SHA, not even when that SHA happens to be a branch's exact current tip. Confirmed with a throwaway test against a small public repo:

```powershell
git clone --branch <current-tip-sha-of-master> <small-public-repo-url> <dir>
# fatal: Remote branch <sha> not found in upstream origin
```

This surfaced when re-running the script days after it was written: `main` had moved past the pinned commit in the meantime, so the mismatch became visible (it's possible the very first "verification" run happened to succeed only because timing made no practical difference at the time — this was never actually re-tested against a truly independent, later clone).

**Fix:** clone the default branch with a plain `west init -m <url> $WS` (no `--mr`), then explicitly `git -C $WS\zephyr checkout $ZEPHYR_REV` before continuing to `west update`. West's manifest-repo clone is never shallow to begin with (it doesn't pass `--depth`), so the full history — including the older pinned commit — is already present locally; the checkout is free.

**Secondary bug found at the same time:** `$ErrorActionPreference = "Stop"` does **not** stop the script when an *external* command (`west`, `git`, `pip`, `pyocd`) fails — it only catches PowerShell-native errors. A failing `west init` was silently followed by `west config`, `west update`, `west zephyr-export`, etc. all failing too, each with an increasingly confusing, unrelated-looking error, instead of the script stopping at the true point of failure. Fixed by wrapping every external call in an `Invoke-Checked { ... }` helper that checks `$LASTEXITCODE` and throws immediately on failure — see the script body above.

### 10c — Name the reproduction package

Tell the user the files that together make up the reproducible installation:
- `C:\zw\reproduce-install.ps1`
- `C:\zw\requirements-lock.txt`
- `C:\zw\app\` (CMakeLists.txt, prj.conf, src\main.c)
- `C:\zw\.vscode\` (see Step 11) — optional, but part of the deliverable if the user works in VS Code

---

## Step 11 — VS Code integration (optional)

For building, flashing, and debugging directly from VS Code, create four files under `C:\zw\.vscode\`. Open `C:\zw` itself as the VS Code workspace root — all paths below assume that.

### 11a — Recommended extensions

`.vscode/extensions.json`:
```json
{
    "recommendations": [
        "ms-vscode.cpptools",
        "marus25.cortex-debug"
    ]
}
```
- **C/C++** (`ms-vscode.cpptools`) — IntelliSense
- **Cortex-Debug** (`marus25.cortex-debug`) — GDB/pyOCD debugging UI

### 11b — Build/flash tasks

`.vscode/tasks.json` calls `west` directly via `${workspaceFolder}/.venv/Scripts/west.exe` — this works **without** activating the venv first, because the venv's `west.exe` embeds the path to its own `python.exe` (a standard property of pip-installed console-script entry points on Windows).

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Zephyr: Build (pristine)",
            "type": "shell",
            "command": "${workspaceFolder}/.venv/Scripts/west.exe",
            "args": [
                "build",
                "-p", "always",
                "-b", "pic32cm_pl10_cnano",
                "-d", "${workspaceFolder}/build",
                "${workspaceFolder}/app",
                "--",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=1"
            ],
            "options": {
                "cwd": "${workspaceFolder}/zephyr",
                "env": { "CMAKE_GENERATOR": "Ninja" }
            },
            "problemMatcher": ["$gcc"],
            "group": { "kind": "build", "isDefault": true },
            "presentation": { "reveal": "always", "panel": "shared" }
        },
        {
            "label": "Zephyr: Build (incremental)",
            "type": "shell",
            "command": "${workspaceFolder}/.venv/Scripts/west.exe",
            "args": [
                "build",
                "-d", "${workspaceFolder}/build"
            ],
            "options": {
                "cwd": "${workspaceFolder}/zephyr",
                "env": { "CMAKE_GENERATOR": "Ninja" }
            },
            "problemMatcher": ["$gcc"],
            "group": "build",
            "presentation": { "reveal": "always", "panel": "shared" }
        },
        {
            "label": "Zephyr: Flash",
            "type": "shell",
            "command": "${workspaceFolder}/.venv/Scripts/west.exe",
            "args": ["flash"],
            "options": { "cwd": "${workspaceFolder}/build" },
            "dependsOn": "Zephyr: Build (pristine)",
            "problemMatcher": [],
            "presentation": { "reveal": "always", "panel": "shared" }
        },
        {
            "label": "Zephyr: Menuconfig",
            "type": "shell",
            "command": "${workspaceFolder}/.venv/Scripts/west.exe",
            "args": ["build", "-d", "${workspaceFolder}/build", "-t", "menuconfig"],
            "options": { "cwd": "${workspaceFolder}/zephyr" },
            "problemMatcher": [],
            "presentation": { "reveal": "always", "panel": "shared" }
        },
        {
            "label": "Zephyr: Clean build dir",
            "type": "shell",
            "command": "powershell",
            "args": ["-NoProfile", "-Command", "Remove-Item -Recurse -Force '${workspaceFolder}/build' -ErrorAction SilentlyContinue"],
            "problemMatcher": [],
            "presentation": { "reveal": "silent", "panel": "shared" }
        },
        {
            "label": "Zephyr: Start GDB Server (pyOCD)",
            "type": "shell",
            "command": "${workspaceFolder}/.venv/Scripts/pyocd.exe",
            "args": [
                "gdbserver",
                "-t", "pic32cm6408pl10048",
                "-f", "2000000",
                "--elf", "${workspaceFolder}/build/zephyr/zephyr.elf"
            ],
            "isBackground": true,
            "problemMatcher": {
                "pattern": {
                    "regexp": "^(?:NEVER_MATCHES_ANYTHING)$",
                    "file": 1,
                    "location": 2,
                    "message": 3
                },
                "background": {
                    "activeOnStart": true,
                    "beginsPattern": ".*Target type is.*",
                    "endsPattern": ".*GDB server listening on port.*"
                }
            },
            "presentation": { "reveal": "always", "panel": "dedicated", "clear": true }
        }
    ]
}
```
The pristine build task passes `-DCMAKE_EXPORT_COMPILE_COMMANDS=1` so `build/compile_commands.json` gets (re-)generated on every full build — that's what makes IntelliSense (Step 11d) work correctly. The Flash task depends on the pristine build task, so `Ctrl+Shift+B` → Flash always ships a fresh binary.

The last task, *Zephyr: Start GDB Server (pyOCD)*, is a long-running background task (`isBackground: true`) with a "never matches" dummy problem pattern plus a `background.endsPattern` that watches for pyOCD's own `GDB server listening on port` line to mark the task ready. It exists specifically to work around a Cortex-Debug bug — see 11c below.

### 11c — Debug configuration

`.vscode/launch.json` — two [Cortex-Debug](https://github.com/Marus/cortex-debug) configurations using `"servertype": "pyocd"`:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            // Deliberately no "preLaunchTask" here — see the warning below.
            "name": "Zephyr: Flash & Debug (pyOCD) - build first!",
            "type": "cortex-debug",
            "request": "launch",
            "cwd": "${workspaceFolder}",
            "executable": "${workspaceFolder}/build/zephyr/zephyr.elf",
            "servertype": "pyocd",
            "serverpath": "${workspaceFolder}/.venv/Scripts/pyocd.exe",
            "device": "pic32cm6408pl10048",
            "interface": "swd",
            "serverArgs": ["-f", "2000000"],
            "armToolchainPath": "C:/Users/<user>/zephyr-sdk-<version>/gnu/arm-zephyr-eabi/bin",
            "toolchainPrefix": "arm-zephyr-eabi",
            "runToEntryPoint": "main",
            "showDevDebugOutput": "raw"
        },
        {
            // postAttachCommands: force a fresh "monitor reset halt" right after
            // attaching - see the warning further below for why this matters.
            "name": "Zephyr: Attach & Debug (pyOCD, no reflash)",
            "type": "cortex-debug",
            "request": "attach",
            "cwd": "${workspaceFolder}",
            "executable": "${workspaceFolder}/build/zephyr/zephyr.elf",
            "servertype": "pyocd",
            "serverpath": "${workspaceFolder}/.venv/Scripts/pyocd.exe",
            "device": "pic32cm6408pl10048",
            "interface": "swd",
            "serverArgs": ["-f", "2000000"],
            "armToolchainPath": "C:/Users/<user>/zephyr-sdk-<version>/gnu/arm-zephyr-eabi/bin",
            "toolchainPrefix": "arm-zephyr-eabi",
            "postAttachCommands": ["monitor reset halt"],
            "showDevDebugOutput": "raw"
        },
        {
            // NOTE: even this workaround config can still hit sporadic SWD "FAULT ACK"
            // errors during a session - see the warning further below. It's the most
            // reliable of the three VS Code configs but not 100% reliable on this board.
            // Also uses postAttachCommands - see the warning below.
            "name": "Zephyr: Debug (attach to running GDB server) - start server task first!",
            "type": "cortex-debug",
            "request": "attach",
            "cwd": "${workspaceFolder}",
            "executable": "${workspaceFolder}/build/zephyr/zephyr.elf",
            "servertype": "external",
            "gdbTarget": "localhost:3333",
            "armToolchainPath": "C:/Users/<user>/zephyr-sdk-<version>/gnu/arm-zephyr-eabi/bin",
            "toolchainPrefix": "arm-zephyr-eabi",
            "postAttachCommands": ["monitor reset halt"],
            "showDevDebugOutput": "raw"
        }
    ]
}
```

Key points, all verified against the [Cortex-Debug attribute reference](https://github.com/Marus/cortex-debug/blob/master/debug_attributes.md):
- `serverArgs: ["-f", "2000000"]` — 2 MHz, raised from `board.cmake`'s conservative 100 kHz default after verifying it stable (flash + multi-cycle debug session, no faults; see the note after this list). Without an explicit `-f`, pyOCD picks its own default frequency, which can fail the same way as the connect issues in Step 8/Troubleshooting.
- `armToolchainPath` + `toolchainPrefix: "arm-zephyr-eabi"` point Cortex-Debug at the **SDK's own** `arm-zephyr-eabi-gdb.exe` (found at `<SDK path>\gnu\arm-zephyr-eabi\bin\`) — a generic `arm-none-eabi-gdb` (Cortex-Debug's default `toolchainPrefix`) will not exist in this lean SDK install.
- `serverpath` points at the pinned pyOCD 0.43.0 inside `.venv` — leaving this unset would let Cortex-Debug fall back to a `pyocd` found elsewhere on PATH, silently reintroducing the 0.44.1 bug from Step 8.

> **Debug frequency raised to 2 MHz.** `board.cmake` pins `west flash` at a conservative 100 kHz (unchanged — see Step 8), but this workspace's own tooling (VS Code tasks/configs, `reproduce-install.ps1`) explicitly overrides `-f` to `2000000` for debugging, verified stable by running the exact same connect → `monitor reset halt` → `break` → `continue` sequence used in Step 11e three loop iterations in a row with no SWD fault, plus a full `pyocd flash` at 2 MHz. If you ever see faults again after this change, drop back toward 100 kHz as the first troubleshooting step — but there was no evidence of instability at 2 MHz on this board/probe combination when tested.

> ⚠️ **Do not add `"preLaunchTask": "Zephyr: Build (pristine)"` to the launch config**, even though it seems like the obvious way to always debug a fresh build. VS Code's `preLaunchTask` wait is unreliable for a build this long — there are multiple confirmed upstream bugs where the debug adapter starts before the task has actually finished (e.g. [microsoft/vscode#54397](https://github.com/Microsoft/vscode/issues/54397), [#263017](https://github.com/microsoft/vscode/issues/263017), [#136527](https://github.com/microsoft/vscode/issues/136527)). Reproduced consistently on this project: `west`/`cmake`/`ninja` processes were still running (confirmed via `Get-Process`) at the exact moment Cortex-Debug threw `Unable to find executable file at ...zephyr.elf` — twice, on fresh attempts, not just when a task was already running from before. **Correct workflow instead:** run *Zephyr: Build (pristine)* or *Zephyr: Flash* manually via Terminal → Run Task, wait for it to actually finish (watch the terminal output settle, e.g. the `Linking C executable zephyr\zephyr.elf` line), *then* start the debug session. Once flashed, prefer the *Attach & Debug (pyOCD, no reflash)* config — it never triggers a build, so this race can't happen at all.

> ⚠️ **If either pyOCD-servertype config fails with `PyOCD: GDB Server Quit Unexpectedly` or `Failed to launch PyOCD GDB Server: Timeout`**, that's a separate, currently-unresolved **Cortex-Debug 1.12.1-on-Windows bug**: the pyOCD server actually starts and reaches `GDB server listening on port 3333` correctly (confirmed independently on this project by running the exact same `pyocd gdbserver` + `gdb` `load`/`reset halt` sequence by hand — it completed without any error), but Cortex-Debug's own detection of "server is ready" fails or times out. Matches multiple open upstream reports with the same environment (Windows, Cortex-Debug 1.12.1, pyOCD servertype): [Marus/cortex-debug#711](https://github.com/Marus/cortex-debug/issues/711), [#1222](https://github.com/Marus/cortex-debug/issues/1222), [#535](https://github.com/Marus/cortex-debug/issues/535). No fix is documented upstream. **Workaround: bypass Cortex-Debug's server auto-launch entirely** using `servertype: "external"`:
> 1. Run task *Zephyr: Start GDB Server (pyOCD)* (Terminal → Run Task) — a long-running background task that starts `pyocd gdbserver` directly and stays open in its own terminal panel.
> 2. Wait for `GDB server listening on port 3333` to appear in that terminal.
> 3. Start the *Zephyr: Debug (attach to running GDB server)* config from the Run and Debug panel — it just does a plain `target extended-remote localhost:3333`, skipping Cortex-Debug's problematic server-launch/detection path completely.
> 4. When done, stop the debug session, then separately stop the GDB server task (trash-can icon on its terminal panel, or `Terminal: Kill the Active Terminal Instance`) — it doesn't stop itself.

> ⚠️ **Root cause of intermittent `FAULT ACK` errors while debugging (now fixed via `postAttachCommands`):** this board's SWD port becomes intermittently inaccessible while the CPU is asleep — `main()`'s `k_msleep()` calls put the Cortex-M0+ into a WFI idle state, and status polls issued while the core is asleep (both by Cortex-Debug's live UI updates *and* by pyOCD's own gdbserver while a plain `gdb` session is connected and running — **this is not Cortex-Debug-specific**, it was reproduced with bare `gdb` too) fail with `Memory transfer fault (SWD/JTAG communication failure (FAULT ACK))` at `0xE000EDF0` (DHCSR). Lowering the SWD frequency (100 kHz → 50 kHz) did **not** help, ruling out a simple clock-speed cause. The actual trigger is **attaching to a target that's already mid-way through sleep/wake cycles** — polling while it happens to be asleep faults, and once one poll faults the GDB session can get desynced (`Could not read registers; remote failure reply '01'`) even after the core wakes up again.
>
> **Fix, confirmed working:** issue `monitor reset halt` immediately after attaching, *before* setting any breakpoints or continuing. This puts the CPU at the reset vector — zero sleep cycles have happened yet — so execution reaches any breakpoint in `main()` well before the first `k_msleep()` call, avoiding the fault window entirely. Both attach-type launch configs above now do this automatically via `"postAttachCommands": ["monitor reset halt"]`. Doing the same by hand in plain `gdb` (see Step 11e) is equally effective — the fix is "always reset before continuing", not "avoid VS Code".

> ⚠️ **Breakpoint tip that matters more now that resets happen automatically:** breaking on `while (1) {` itself still only fires once (see the Troubleshooting table) — break on a line **inside** the loop body instead, e.g. `main.c:64` (`if (blink_ms > 0) {`).

> ⚠️ **Machine-specific paths:** `armToolchainPath` hardcodes the SDK location (`C:\Users\<user>\zephyr-sdk-<version>\...`), which is user- and machine-specific (`west sdk install` installs to the home directory by default). When reproducing this setup elsewhere via `reproduce-install.ps1`, update this path in `launch.json` to match the new machine — it is **not** parameterized by the reproduction script.

### 11d — IntelliSense

`.vscode/c_cpp_properties.json`:
```json
{
    "version": 4,
    "configurations": [
        {
            "name": "Zephyr (PIC32CM PL10)",
            "compileCommands": "${workspaceFolder}/build/compile_commands.json",
            "compilerPath": "C:/Users/<user>/zephyr-sdk-<version>/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc.exe",
            "cStandard": "gnu11",
            "cppStandard": "gnu++17",
            "intelliSenseMode": "gcc-arm"
        }
    ]
}
```
`compileCommands` needs a `build/compile_commands.json` to exist — it's generated by the pristine build task (11b), so run that once before opening a source file for the first time. `compilerPath` uses the SDK's own ARM GCC (not a system compiler) so IntelliSense picks up ARM-specific built-in defines and include paths correctly — same machine-specific-path caveat as 11c applies.

### 11e — Command-line debugging (also useful outside VS Code)

Same underlying tooling as 11c, no VS Code required. As documented in the warning in 11c, always issue `monitor reset halt` right after connecting and before `continue`-ing — attaching to a target that's already mid-sleep-cycle (from `k_msleep()`) reproducibly triggers SWD faults, in plain `gdb` just as much as in Cortex-Debug. Reset-first avoids the problem entirely, verified working end-to-end on this project (connect, reset halt, hit a breakpoint inside the blink loop, no SWD fault).

**Terminal 1 — start the GDB server and leave it running:**
```cmd
C:\zw\.venv\Scripts\activate.bat
pyocd gdbserver -t pic32cm6408pl10048 -f 2000000 --elf C:\zw\build\zephyr\zephyr.elf
```
Wait for `GDB server listening on port 3333`.

**Terminal 2 — connect with the SDK's own GDB:**
```cmd
C:\Users\<user>\zephyr-sdk-<version>\gnu\arm-zephyr-eabi\bin\arm-zephyr-eabi-gdb.exe C:\zw\build\zephyr\zephyr.elf
```
At the `(gdb)` prompt:
```
target extended-remote localhost:3333
monitor reset halt
break main.c:64
continue
```
(`break main.c:64` breaks inside the `while (1)` loop body — a bare `break main` or a breakpoint on the `while (1) {` line itself only fires once, at the very first loop entry, and will never trigger again on a plain `attach`; see the Troubleshooting table.)

Useful commands from there: `next`/`n` (step over), `step`/`s` (step into), `continue`/`c`, `print <var>`, `backtrace`/`bt`, `break <file>:<line>`. To reflash before debugging instead of just attaching: `monitor reset` → `load` → `monitor reset halt` (in that order, right after `target extended-remote`). `quit` exits GDB; the server in Terminal 1 keeps running until you `Ctrl+C` it there.

---

## Appendix A — Detailed memory analysis

> **Note:** these figures capture the project's original **bare-blinky** build progression
> (single `main.c`, `console_getline()`, one app thread). The app has since grown an ADC
> read/stream, a reset, per-domain source files, a `console_getchar()`-based parser with
> command history, and three application threads — its current totals are higher (see the
> README's "Memory usage" section for the live numbers). The analysis below stands as the
> record of *how the lean baseline was reached*; it is not the current footprint.

This appendix has the full history and breakdown behind the original lean baseline.

**Across the four build versions the original blinky went through:**

| Version | RAM | Flash |
|---|---|---|
| Original Zephyr shell (`CONFIG_SHELL`, defaults) | 96.48% (7,904 B) | 69.69% (42,816 B) |
| Shell, trimmed (`SHELL_STACK_SIZE=1536`, history/getopt off) | 83.50% (6,840 B) | 66.56% (40,892 B) |
| `console_getline()`, hand-written prompt + `help` | 52.25% (4,280 B) | 27.13% (16,668 B) |
| **Current** (+ `ISR_STACK_SIZE=1024`, `MAIN_STACK_SIZE=512`) | **33.50% (2,744 B)** | **27.13% (16,668 B)** |

**Where the current build's usage goes** (top-level `ram_report`/`rom_report` categories):

| Module | RAM | Module | Flash |
|---|---|---|---|
| kernel | 2,230 B (81.2%) | drivers | 5,240 B (31.5%) |
| subsys (console) | 308 B (11.2%) | kernel | 4,712 B (28.3%) |
| app (`main.c`) | 60 B (2.2%) | libc runtime¹ | 2,551 B (15.3%) |
| drivers | 97 B (3.5%) | unattributed² | 1,177 B (7.1%) |
| lib | 40 B (1.5%) | lib | 1,092 B (6.6%) |
| arch | 4 B (0.1%) | arch | 1,014 B (6.1%) |
| | | app (`main.c`) | 420 B (2.5%) |
| | | subsys (console) | 128 B (0.8%) |

¹ Compiler-generated runtime helpers with no source-file attribution (division, `memcpy`, printf helpers).
² `rom_report`'s own `(hidden)` bucket — mostly inlined/optimized code without a clear symbol source.

RAM is still dominated by kernel scaffolding (81%), but that scaffolding itself was cut:
`CONFIG_ISR_STACK_SIZE` (2048→1024 B) and `CONFIG_MAIN_STACK_SIZE` (1024→512 B) — both
fixed baseline costs independent of the app. **This is riskier than trimming the shell's
per-thread stack**: `ISR_STACK_SIZE` is shared by *every* interrupt in the system (GPIO,
UART RX, SysTick/timer) plus kernel init, so its worst-case need is harder to bound, and
an overflow is more likely to silently corrupt memory than fault cleanly. Verified with a
deliberately adversarial `pyserial` script — `led blink 10` (10 ms timer-ISR period, the
fastest realistic interrupt load here) running while firing 15 rapid `help` calls (the
deepest `printk`/format-string stack use in the app) over UART — no fault, no corrupted
output. This is an empirical stress test of *this app's* actual interrupt surface, **not a
formal worst-case proof**; re-verify with a similar test if you add new interrupt sources
later. Flash's biggest category is the SERCOM/clock/GPIO drivers now that the shell is
gone. The app itself (`main.c`) is tiny either way: 60 B RAM, 420 B flash for the entire
blink-timer + command-parser + prompt + help logic.

**For context, a comparable CMSIS/RTX5 application** on the same chip
(`C:\work\Bukarest\3_Mi_CMSIS\CNANO_MCHP_Driver_Example` — GPIO LED via a `k_timer`-style
thread, ADC polling, button-interrupt-to-USART, three CMSIS-RTOS2 threads + a mutex; no
interactive command parser), read directly from its linker map
(`CNANO_MCHP_Driver_Example.axf.map`, `LR_ROM`/`RW_RAM`+heap+stack region sizes):

| | Zephyr (this project) | CMSIS/RTX5 (Bukarest project) |
|---|---|---|
| RAM | 4,280 B (**52.25%** of 8 KB) | 4,736 B (**57.81%** of 8 KB) |
| Flash | 16,668 B (**27.13%** of 60 KB\*) | 20,568 B (**31.39%** of 64 KB\*) |
| Threads | 1 (`main`) + a `k_timer` callback | 3 (LED/ADC/button) + a mutex |
| Interactive commands | yes (`led on/off/toggle/blink`, `help`) | no |

\* Small capacity-assumption difference between the two projects' linker scripts (64 KB
vs. Zephyr's 60 KB `board.cmake` value) — both target the same physical chip
(PIC32CM6408PL10048). Zephyr's RAM here is the `console_getline()` build *before* the
kernel-stack trim above (4,280 B) — the current shipped build is smaller still (2,744 B,
33.50%), widening this gap further.

Despite doing *more* (3 RTOS threads, ADC, USART) and offering *less* (no interactive
commands), the CMSIS/RTX5 build isn't meaningfully leaner than this Zephyr build — RAM is
actually slightly higher. This suggests the RAM cost this project chased down was
specifically the **Zephyr shell subsystem's overhead**, not something inherent to Zephyr
itself: with the shell replaced by `console_getline()`, Zephyr lands in the same ballpark
as a hand-tuned CMSIS/RTX5 equivalent on this chip — and pulls ahead once the kernel
stacks are trimmed too.

**Cost per extra thread (concurrency check):** the comparison above isn't quite fair to
Zephyr's threading model: this app only runs **one** thread (`main`) — `blink_timer`'s
callback fires in interrupt context, not a second thread — while the CMSIS/RTX5 app
actually runs three. To measure Zephyr's per-thread cost directly, three throwaway
`K_THREAD_DEFINE(..., 256, ...)` threads were added temporarily, built, measured with
`ram_report`, then removed again (not part of the shipped app; measured before the
kernel-stack trim above, so the baseline row is 4,280 B — the delta itself is unaffected
by that trim and still applies):

| | RAM | Δ per thread |
|---|---|---|
| Baseline (1 thread) | 4,280 B (52.25%) | — |
| +3 dummy threads (256 B stack each) | 5,384 B (65.72%) | **368 B** (256 B stack + 112 B thread control block, exactly as declared — no hidden overhead) |

That's a modest, predictable per-thread cost, likely comparable to RTX5's (typically a
few hundred bytes depending on stack size). The real difference between the two RTOSes on
this chip isn't per-thread cost — it's Zephyr's **fixed baseline** (interrupt stack + main
stack, present regardless of thread count). With the current, smaller 2,744 B baseline
there's room for roughly 14-15 more 256 B threads (vs. ~8-10 estimated against the
pre-trim baseline) before RAM becomes the constraint — fine for a handful of concurrent
tasks, but the fixed Zephyr baseline (not per-thread cost) is what limits how far this
scales on an 8 KB part.

## Appendix B — Moving the version pin forward (`west update` internals)

This workspace pins everything on purpose (see Step 3 and the Constraints section). This
appendix explains exactly what `west update` does and doesn't cover, for when you
deliberately want to move a pin forward later instead of just reproducing the current
state.

`west update` only syncs the **module projects** listed in the manifest — `hal_microchip`,
`cmsis`, `cmsis_6`, `picolibc` (after `manifest.group-filter` and `manifest.project-filter`
are applied) — to whatever revisions are pinned in the `west.yml` that's currently checked
out inside `zephyr/`. Concretely, for each project that survives the filters, it clones/
fetches and checks out the exact commit/tag the manifest specifies; anything filtered out
is never touched. Run it twice with no manifest changes and it's a no-op.

It does **not** touch `zephyr/` itself. That repo is pinned to one exact commit
(`git checkout <SHA>`, see `reproduce-install.ps1`'s `$ZEPHYR_REV`) — not managed by the
west-update mechanism at all. So if Zephyr mainline gains a new PL10 peripheral driver
after that commit, `west update` will never fetch it, no matter how often you run it — the
driver source (`drivers/...`, DTS bindings) lives in `zephyr/` proper, not in a module.

To actually pull in newer mainline code:

1. Move the pin forward yourself: `Set-Location C:\zw\zephyr; git fetch upstream; git
   checkout <newer-commit-or-tag>` (detached HEAD, same as initial setup — not `git pull`
   on a branch).
2. *Then* run `west update` — it now reads the `west.yml` from the new commit, and pulls
   any module revision bumps that commit requires (e.g. a `hal_microchip` bump backing the
   new driver).
3. Check whether the new driver needs a module your `project-filter` currently excludes —
   if so, widen it for that one project (see Step 3) before rebuilding.
4. Pristine rebuild (`-p always`) and re-check `ram_report`/`rom_report` — see Appendix A;
   there's very little headroom.
5. If the new driver adds interrupt sources, re-run the stress test described in the
   `ISR_STACK_SIZE` discussion in Step 7 before trusting the stack-size tuning still holds.

This is a deliberate, riskier step, not a routine `west update` — it can drag along other
module revision changes and SDK-compatibility shifts, so treat it as its own change to
test end-to-end (build + flash + interaction), not a drive-by fetch.

---

## Troubleshooting (consult on errors)

First map the error to a pipeline stage: **west → CMake → Kconfig/Devicetree → Compiler → Linker.** Then target it:

| Symptom | Cause | Fix |
|---|---|---|
| "Path too long" / `CMAKE_OBJECT_PATH_MAX` | Object path > 250 characters (Windows) | Keep workspace/build dir short (`C:\zw`, `-d C:\zw\build`); enable Windows long paths (`LongPathsEnabled=1`, admin+reboot); `git config --system core.longpaths true` |
| CMake looks for `nmake` / "no such file" | Fell back to NMake instead of Ninja | Ensure `ninja` is on PATH; `$env:CMAKE_GENERATOR="Ninja"`; restart |
| "running scripts is disabled" | PowerShell execution policy | `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`; or run the script via `powershell -ExecutionPolicy Bypass -File` |
| "Invalid character escape '\p'" | Backslash path in a CMake string | Update west/CMake; use paths without special characters (`C:\zw`) |
| "SDK version not supported" / toolchain missing | SDK missing/outdated/wrong target | `west sdk install -t arm-zephyr-eabi`; match the SDK version to the repo state |
| `west: command not found` / "invalid choice: 'build'" | venv not active / outside the workspace / stale west | Activate the venv; build from within `C:\zw`; `pip install -U west` |
| Build very slow | Defender real-time scan | `Add-MpPreference -ExclusionPath 'C:\zw'` (only for a trusted path) |
| Undefined Kconfig symbol / missing DT node | Doesn't match the board | `west build -t menuconfig`; check board DTS/binding; inspect the generated `build\zephyr\zephyr.dts` |
| `led0` alias missing | Board uses a different LED alias | Check the board DTS for the LED alias/label, adjust `LED0_NODE` |
| FLASH/RAM "region overflowed" | Too many configs enabled | Trim configs; `west build -t rom_report` |
| RAM usage near 100% even for a trivial app on an 8 KB-RAM part | If `CONFIG_SHELL=y` is enabled: Zephyr's shell subsystem is RAM-heavy by default (2048 B thread stack + 512 B history heap out of the box) — this is exactly why this app dropped the shell entirely in favor of `console_getline()` (see Step 7) | `west build -t ram_report` to see the actual breakdown. If you re-add the shell and need it lean, trim `CONFIG_SHELL_STACK_SIZE` and disable `CONFIG_SHELL_HISTORY`/`CONFIG_SHELL_GETOPT`; otherwise consider `console_getline()` instead, as this app now does |
| Hard fault (register dump, `Faulting instruction address`) only when running `help`, not your own shell commands (only applies if you re-add `CONFIG_SHELL`, which this app no longer uses) | `CONFIG_SHELL_STACK_SIZE` cut too low — `help` walks the whole command tree and needs more stack depth than a single simple command | Raise `CONFIG_SHELL_STACK_SIZE` (1536 was verified stable in this app's earlier shell-based version); test `help` explicitly after any reduction, not just your own commands |
| Kernel scaffolding (`z_interrupt_stacks`/`z_main_stack`) still dominates RAM after the shell is gone | `CONFIG_ISR_STACK_SIZE` (default 2048) and `CONFIG_MAIN_STACK_SIZE` (default 1024) are fixed baseline costs independent of app size | Trim both (this app: 1024/512, verified via a `pyserial` stress test — fast timer-ISR load concurrent with repeated `help` calls, see Step 7's note); `ISR_STACK_SIZE` is shared by *every* interrupt in the system, so re-verify with a similar stress test if you add new interrupt sources later — don't just assume the same values stay safe |
| `west init`: `PermissionError`/`WinError 5` while moving/deleting `.west\manifest-tmp` | Transient file lock (likely AV real-time scan) on a freshly cloned `.git\objects\pack\*.idx` | Check the clone state (`git -C C:\zw\zephyr status`/`log -1`) — usually already complete; add `.west\config` manually if needed (see Step 2) instead of re-cloning |
| `west list-groups`: "unknown command" | The command no longer exists in current west versions | Read groups/projects directly from `west.yml` (`Select-String -Pattern "groups:"`) instead of `west list-groups` |
| `west update` pulls in dozens of irrelevant modules (mbedtls, lvgl, hostap, trusted-firmware-…) | `group-filter` is coarse-grained, affects whole groups only; mandatory modules from fs/crypto/tee/debug/tools/lib stay active regardless | Additionally set `manifest.project-filter` as an allow-list (see Step 3); manually delete already-cloned, no-longer-active `modules\*` folders afterward |
| `west packages pip --install` fails on `hidapi` with "Microsoft Visual C++ 14.0 or greater is required" | Installs Zephyr's entire (test/compliance) requirements set instead of just the build base, which forces an older `hidapi` version without a matching Windows wheel | `pip install -r zephyr\scripts\requirements-base.txt` instead of `west packages pip --install`; install pyOCD separately with `pip install pyocd` (without the test-package constraints, pip picks a newer `hidapi` with a wheel) |
| `west sdk install`: "Zephyr SDK setup requires '7z' … in the PATH", afterward `sdk_gnu_toolchains` stays empty | The SDK's own `setup.cmd` needs `7z` literally on PATH (not just 7-Zip installed); without a successful setup, the GNU toolchain is never fetched | `$env:PATH = 'C:\Program Files\7-Zip;' + $env:PATH`, then run `west sdk install -t arm-zephyr-eabi -H` again (detects the existing download, fetches only the missing toolchain) |
| `pyocd flash`/`west flash`: "SWD/JTAG communication failure (FAULT ACK)" or "(No ACK)", regardless of connect mode/frequency | **pyOCD 0.44.1 is broken for the PIC32CM PL10** | `pip install "pyocd==0.43.0"`, then flash again — fixes it directly with no other changes |
| Flashing fails even though the board is detected and a previous flash attempt stopped the LED | SWD connection was briefly established (triggering a reset) but the session wasn't cleanly set up — more transient than structural | Briefly unplug/replug the board, retry, before assuming a cable/hardware problem |
| Cortex-Debug (any of the three launch.json configs): "Unhandled exception processing RSP command ... FAULT ACK" mid-session, breakpoint never hit or session dies after a while | Cortex-Debug polls the core status register (DHCSR) continuously in the background to keep its UI live; this board's SWD link can't reliably sustain that continuous load (reproduced at both 100 kHz and 50 kHz — not a clock-speed issue) | Unplug/replug the board; use plain command-line GDB instead (Step 11e), which only talks to the target on demand and has been reliable where Cortex-Debug faults |
| Breakpoint on `while (1) {` (or `break main` landing on a loop-entry line) never triggers again after the first hit | That source line has no associated machine code on subsequent iterations — the compiled backward branch jumps straight to the first statement in the loop body, not back to the `while` line itself; doubly true when *attaching* to an already-running target, since the one-time entry already happened before you connected | Set the breakpoint on a line **inside** the loop body that's genuinely re-executed every iteration (e.g. `break main.c:64` for this app's `if (blink_ms > 0) {`) |
| `gdb`: `target extended-remote localhost:3333` fails immediately with `Remote communication error. Target disconnected: error while reading: No such file or directory` (no `Client 1 connected` line ever appears in the server's terminal) | A **second** `pyocd gdbserver` instance is already running (e.g. a VS Code task from Step 11c's workaround left running, plus a manually-started one for Step 11e) — two instances can't share the same USB debug probe/port | Check `Get-Process \| Where-Object { $_.ProcessName -match 'pyocd' }`; if more than one, close **all** of them (Ctrl+C in every terminal running `pyocd gdbserver`, and/or stop the VS Code task via its terminal's trash-can icon) and start exactly **one** fresh instance before reconnecting |
| `reproduce-install.ps1`: `fatal: Remote branch <sha> not found in upstream origin` during `west init`, then a cascade of unrelated-looking errors afterward (`MalformedConfig`, `unknown command "zephyr-export"`, `Set-Location: Cannot find path ...\zephyr`) | `west init -m <url> --mr <SHA>` doesn't work — `git clone --branch` only resolves ref names, never a raw commit SHA (see Step 10d); the cascade happens because `$ErrorActionPreference = "Stop"` doesn't stop the script on a failing *external* command | Already fixed in this repo's `reproduce-install.ps1` (clone default branch, then `git checkout <sha>`, plus an `Invoke-Checked` wrapper around every external call) — if you see this, you're running a stale copy of the script; get the current one |

**Agent rule:** exactly one change per iteration, then `west build -p always` (pristine), so no stale CMake cache skews the result.

---

## Completion checklist

- [ ] `C:\zw` set up lean (ARM toolchain only, only the necessary modules via `group-filter` **and** `project-filter`, shallow clone)
- [ ] Board target for PL10 verified and used
- [ ] Blinky with LED command interface built and flashed
- [ ] `pyocd` version checked: **0.43.0**, not 0.44.1
- [ ] Commands `led on/off/toggle/blink <ms>` work on the board (manually or automated via `pyserial`), including an unrecognized-command case
- [ ] RAM/flash usage checked with `ram_report`/`rom_report` and sane for the target's memory size (not just "does it fit")
- [ ] `reproduce-install.ps1` produced with **pinned** values (including `project-filter`, `pyocd==0.43.0`, `-H` for the SDK, 7-Zip PATH safeguard)
- [ ] Reproduction package named (script + lock file + app folder)
- [ ] VS Code integration set up (`tasks.json`, `launch.json`, `c_cpp_properties.json`, `extensions.json`) and build/flash tasks verified to actually run
