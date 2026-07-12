# PIC32CM PL10 Curiosity Nano — Lean Zephyr Blinky

> ## Got this as `pic32cm-pl10-reproduction.zip`? Start here.
>
> This zip is a **self-contained reproduction package**: the application source, VS Code
> integration, all docs, and one pinned script that rebuilds the *entire* Zephyr workspace
> around them — full RTOS clone, HAL modules, Python venv, toolchain — on any Windows
> machine. It does **not** contain the Zephyr source, modules, venv, or build output
> themselves; the script fetches all of that. Extract the zip, run one script, the rest
> happens on its own:
>
> ```powershell
> # 1. Extract the zip into an empty target directory, e.g. C:\zw2
> Expand-Archive -Path pic32cm-pl10-reproduction.zip -DestinationPath C:\zw2
>
> # 2. Move into that directory and run the script with a relative path
> Set-Location C:\zw2
> powershell -ExecutionPolicy Bypass -File reproduce-install.ps1
> ```
>
> Step 2 matters: the script determines its target directory from `$PSScriptRoot` — the
> actual location of the `.ps1` file being run, not your terminal's starting directory.
> `cd` into the extracted folder first and call the script by its bare filename (not a full
> path to some other copy) so `$PSScriptRoot` — and therefore everything the script
> builds — resolves to *that* folder. Calling a copy elsewhere by full path (e.g. an old
> `C:\zw\reproduce-install.ps1` left over from a previous setup) builds into *that* folder
> instead, which is a common mix-up if more than one copy of this project exists on the
> same machine.
>
> That's it. The script clones the pinned Zephyr revision, fetches only the four modules
> this board needs, installs the pinned SDK and Python packages, builds the app, and
> flashes the board — finishing with `Reproduction complete.` in green. Expect roughly
> 10-20 minutes depending on network speed (most of it is the Zephyr/module clone and SDK
> download).
>
> **Two things the script does *not* do for you:**
> - **Prerequisites** — Python (3.12+), Git, Ninja, CMake, and 7-Zip must already be on
>   `PATH` on the target machine. The script only checks for them and stops with a clear
>   error if one is missing; it does not install them.
> - **Board connection** — the PIC32CM PL10 Curiosity Nano must be plugged in via USB
>   *before* the script reaches its last step (`pyocd flash`). Everything before that
>   (venv, workspace, SDK, build) succeeds without the board attached.
>
> See ["Reproducing this setup elsewhere"](#reproducing-this-setup-elsewhere) below for
> exactly what's pinned and why, and the VS Code section for one caveat about hardcoded SDK
> paths if you also want debugging to work on a different machine.

A minimal, from-scratch [Zephyr RTOS](https://www.zephyrproject.org/) workspace for the
**Microchip PIC32CM PL10 Curiosity Nano** board: a blinky application with a minimal
serial command interface for interactively controlling the LED, plus a pinned,
non-interactive script that reproduces this exact installation elsewhere.

> The app originally used the full Zephyr shell subsystem; it was replaced with a
> lightweight `console_getline()`-based parser once RAM profiling showed the shell alone
> using ~40-47% of this part's 8 KB of RAM. See "Design principle" below and `RUNBOOK.md`
> → Step 7 for the full rationale and numbers.

Built by following [`RUNBOOK.md`](RUNBOOK.md), which also documents every pitfall hit
along the way — read it if you need to redo or extend this setup.

## Build & flash — `cmd.exe`, from `C:\zw`

```cmd
C:\zw\.venv\Scripts\activate.bat

cd C:\zw\zephyr
set CMAKE_GENERATOR=Ninja
west build -p always -b pic32cm_pl10_cnano -d C:\zw\build C:\zw\app

cd C:\zw\build
west flash
```

Activating the venv first is required — calling `west.exe` by its full path without
activating works for `build` but not for `flash` (its pyOCD runner looks for `pyocd` on
`PATH`, not by full path). See "Quick start" below for the PowerShell equivalent and more
detail.

## What's here

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

## Hardware

- Board: **PIC32CM PL10 Curiosity Nano** (Zephyr board name `pic32cm_pl10_cnano`, SoC `pic32cm6408pl10048`, Arm Cortex-M0+)
- On-board debugger: Microchip **nEDBG** (CMSIS-DAP over USB), also exposes a virtual COM port for the serial console
- No external debug probe needed — just a USB data cable

## Quick start (this machine, already set up)

```powershell
& C:\zw\.venv\Scripts\Activate.ps1
Set-Location C:\zw\zephyr
$env:CMAKE_GENERATOR = "Ninja"
west build -p always -b pic32cm_pl10_cnano -d C:\zw\build C:\zw\app
```

Flash (from the build directory, so `west flash` picks up the board's pinned SWD
frequency from `board.cmake` automatically):

```powershell
Set-Location C:\zw\build
west flash
```

**In `cmd.exe` instead of PowerShell**, use `activate.bat` (not `Activate.ps1`, which is
PowerShell-only):

```cmd
C:\zw\.venv\Scripts\activate.bat
cd C:\zw\build
west flash
```

> ⚠️ Calling `C:\zw\.venv\Scripts\west.exe` by its full path *without* activating the venv
> first works for `west build`, but **not** for `west flash` — it fails with `FATAL ERROR:
> required program pyocd not found; install it or add its location to PATH`. Unlike
> `west.exe` itself (a generated entry point that embeds its own `python.exe` path),
> `west flash`'s pyocd runner shells out to a bare `pyocd` command and relies on `PATH` to
> find it. Activating the venv (`activate.bat` in cmd, `Activate.ps1` in PowerShell) puts
> `.venv\Scripts` on `PATH` and fixes this — after that, plain `west` (no full path)
> works for the rest of that session.

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

**Debug** — Run and Debug panel (`Ctrl+Shift+D`):
- *Zephyr: Flash & Debug (pyOCD) - build first!* — flashes and stops at `main()`
- *Zephyr: Attach & Debug (pyOCD, no reflash)* — attaches to whatever's already running
- *Zephyr: Debug (attach to running GDB server) - start server task first!* — most
  reliable option; see the second warning below

All tasks call `west`/`pyocd` directly from `.venv/Scripts/` (no need to activate the
venv first) and use the pinned `pyocd==0.43.0` explicitly, so Cortex-Debug can't
accidentally pick up a different (broken) pyOCD from `PATH`.

> ⚠️ **Always build first, separately, before debugging.** Neither debug config auto-builds
> (no `preLaunchTask`) — VS Code's preLaunchTask wait is unreliable for a build this long
> and can start the debugger before the build actually finishes, causing a spurious
> "Unable to find executable file" error (reproduced consistently on this project; see
> known upstream bugs [microsoft/vscode#54397](https://github.com/Microsoft/vscode/issues/54397),
> [#263017](https://github.com/microsoft/vscode/issues/263017)). Correct order: run
> *Zephyr: Build (pristine)* or *Zephyr: Flash* via Terminal → Run Task, wait for it to
> actually finish in the terminal, **then** start debugging.

> ⚠️ **If debugging fails with "GDB Server Quit Unexpectedly" or a launch timeout**, that's
> a known, unresolved Cortex-Debug 1.12.1-on-Windows bug — pyOCD's server actually starts
> fine, Cortex-Debug just fails to detect it (see [Marus/cortex-debug#711](https://github.com/Marus/cortex-debug/issues/711),
> [#1222](https://github.com/Marus/cortex-debug/issues/1222)). Confirmed on this project:
> the identical `pyocd gdbserver` + `gdb load`/`reset halt` sequence run by hand completes
> without any error. **Workaround:** run task *Zephyr: Start GDB Server (pyOCD)* (stays
> open in its own terminal), wait for `GDB server listening on port 3333`, then start
> *Zephyr: Debug (attach to running GDB server)* — it skips Cortex-Debug's problematic
> auto-launch/detection path entirely. See `RUNBOOK.md` → Step 11c for the full explanation.

> ⚠️ **Root cause of intermittent `FAULT ACK` faults while debugging, now fixed:** this
> board's SWD port becomes intermittently inaccessible while the CPU is asleep — `main()`'s
> `k_msleep()` calls trigger a WFI idle state, and status polls issued while the core
> happens to be asleep fail with `Memory transfer fault (FAULT ACK)` at the DHCSR register.
> This is **not Cortex-Debug-specific** — it reproduced with plain `gdb` too. The trigger is
> *attaching to a target already mid-sleep-cycle*; lowering the SWD frequency (100 kHz →
> 50 kHz) did not help. **Fix:** both attach-type configs now run
> `"postAttachCommands": ["monitor reset halt"]` automatically, putting the CPU at the reset
> vector (zero sleep cycles elapsed) before you ever set a breakpoint — so execution reaches
> anything in `main()` well before the fault window. Also remember: a breakpoint on
> `while (1) {` itself only fires once (see `RUNBOOK.md` Troubleshooting table) — break on a
> line *inside* the loop body instead, e.g. `main.c:64`.
>
> The same fix applies if you debug via plain command-line GDB instead of VS Code (also
> useful standalone — see `RUNBOOK.md` → Step 11e): always run `monitor reset halt` right
> after connecting, before setting breakpoints or continuing.
> ```cmd
> :: Terminal 1
> C:\zw\.venv\Scripts\activate.bat
> pyocd gdbserver -t pic32cm6408pl10048 -f 2000000 --elf C:\zw\build\zephyr\zephyr.elf
> :: Terminal 2, after Terminal 1 shows "GDB server listening on port 3333"
> C:\Users\<user>\zephyr-sdk-<version>\gnu\arm-zephyr-eabi\bin\arm-zephyr-eabi-gdb.exe C:\zw\build\zephyr\zephyr.elf
> (gdb) target extended-remote localhost:3333
> (gdb) monitor reset halt
> (gdb) break main.c:64
> (gdb) continue
> ```
> Only ever run **one** `pyocd gdbserver` instance at a time — a second one (e.g. a leftover
> VS Code task still running alongside a manually-started one) can't share the USB probe and
> causes a confusing `Remote communication error: ... No such file or directory` on connect.

> ⚠️ `launch.json` and `c_cpp_properties.json` hardcode this machine's SDK install path
> (`C:\Users\<user>\zephyr-sdk-<version>\...`, chosen by `west sdk install`'s default
> location). If you reproduce this setup on another machine via `reproduce-install.ps1`,
> update those two paths in `.vscode/` to match — they are not parameterized by the
> reproduction script. See `RUNBOOK.md` → Step 11 for the full rationale and JSON.

## Reproducing this setup elsewhere

`reproduce-install.ps1` is **portable**: it builds the workspace in whatever directory it
itself is located in (via `$PSScriptRoot`), not a hardcoded path. The easiest way to get
all the files it needs next to it is `pic32cm-pl10-reproduction.zip` — see the callout at
the very top of this README for the two-command extract-and-run flow.

If you don't have the zip (e.g. copying straight from this machine or a network share
instead), the same result is achieved by copying the three reproduction items into a
target directory yourself before running the script:

```powershell
New-Item -ItemType Directory -Force -Path C:\zw | Out-Null
Copy-Item <source>\reproduce-install.ps1  C:\zw\
Copy-Item <source>\requirements-lock.txt  C:\zw\
Copy-Item <source>\app -Destination C:\zw\app -Recurse

Set-Location C:\zw
powershell -ExecutionPolicy Bypass -File reproduce-install.ps1
```

Calling it by relative filename after `Set-Location` (rather than a full path) avoids
accidentally pointing at a different copy of the script elsewhere on the machine — see the
callout at the top of this README for why that matters.

(`<source>` is wherever you staged/transferred the three files from.) The script itself
does **not** fetch or copy `requirements-lock.txt`/`app\` — they must already sit next to
it before it runs, which is exactly what the zip package provides in one step.

This is fully non-interactive and pins every moving part: the exact Zephyr commit, west
version, SDK version, module filters, Python package versions (via
`requirements-lock.txt`), board name, and pyOCD flash target/frequency. It ends with a
build-and-flash verification, so a successful run means the board is left running the
same firmware as this machine.

**Prerequisites on the target machine:** Python (3.12+), Git, Ninja, CMake, and 7-Zip —
all on `PATH`. See `RUNBOOK.md` → "Checking prerequisites" for details and why each one
matters.

## Important: pyOCD version is pinned for a reason

This setup pins **`pyocd==0.43.0`**. Version 0.44.1 cannot establish a working SWD debug
session with this specific board — flashing fails with `SWD/JTAG communication failure
(FAULT ACK)` / `(No ACK)` regardless of connect mode or clock speed. If you ever
`pip install --upgrade pyocd` in this venv, you will likely reintroduce this bug; re-pin
to 0.43.0 (or verify a newer fixed release) if flashing suddenly stops working.

## Design principle: lean by construction

This workspace was deliberately built as narrow as west allows:

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
  parity (see `RUNBOOK.md` → Step 7). See "Memory usage" below for the numbers.

Net result: a full, working toolchain and firmware in a few hundred MB instead of the
several GB a default Zephyr installation would use.

## What `west update` actually does (and doesn't)

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
   if so, widen it for that one project (see "Design principle" above) before rebuilding.
4. Pristine rebuild (`-p always`) and re-check `ram_report`/`rom_report` — see "Memory
   usage" below; there's very little headroom.
5. If the new driver adds interrupt sources, re-run the stress test described in the
   `ISR_STACK_SIZE` discussion in `CLAUDE.md` before trusting the stack-size tuning still
   holds.

This is a deliberate, riskier step, not a routine `west update` — it can drag along other
module revision changes and SDK-compatibility shifts, so treat it as its own change to
test end-to-end (build + flash + interaction), not a drive-by fetch.

## Memory usage

This board has only 8 KB RAM / 60 KB flash, so usage is worth watching, not just
"does it fit." Current build:

| | Used | Capacity | % used |
|---|---|---|---|
| RAM | 2,744 B | 8,192 B | **33.50%** |
| Flash | 16,668 B | 61,440 B | **27.13%** |

Check live numbers with `west build -d C:\zw\build -t ram_report` / `-t rom_report`
(per-symbol breakdown).

**Across the four build versions this project went through:**

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
kernel-stack trim below (4,280 B) — the current shipped build is smaller still (2,744 B,
33.50%), widening this gap further.

Despite doing *more* (3 RTOS threads, ADC, USART) and offering *less* (no interactive
commands), the CMSIS/RTX5 build isn't meaningfully leaner than this Zephyr build — RAM is
actually slightly higher. This suggests the RAM cost this project chased down was
specifically the **Zephyr shell subsystem's overhead**, not something inherent to Zephyr
itself: with the shell replaced by `console_getline()`, Zephyr lands in the same ballpark
as a hand-tuned CMSIS/RTX5 equivalent on this chip — and pulls ahead once the kernel
stacks are trimmed too (see below).

### Cost per extra thread (concurrency check)

The comparison above isn't quite fair to Zephyr's threading model: this app only runs
**one** thread (`main`) — `blink_timer`'s callback fires in interrupt context, not a
second thread — while the CMSIS/RTX5 app actually runs three. To measure Zephyr's
per-thread cost directly, three throwaway `K_THREAD_DEFINE(..., 256, ...)` threads were
added temporarily, built, measured with `ram_report`, then removed again (not part of the
shipped app; measured before the kernel-stack trim below, so the baseline row is 4,280 B
— the delta itself is unaffected by that trim and still applies):

| | RAM | Δ per thread |
|---|---|---|
| Baseline (1 thread) | 4,280 B (52.25%) | — |
| +3 dummy threads (256 B stack each) | 5,384 B (65.72%) | **368 B** (256 B stack + 112 B thread control block, exactly as declared — no hidden overhead) |

That's a modest, predictable per-thread cost, likely comparable to RTX5's (typically a
few hundred bytes depending on stack size). The real difference between the two RTOSes on
this chip isn't per-thread cost — it's Zephyr's **fixed baseline** (interrupt stack + main
stack, present regardless of thread count — see below for how far *that* was trimmed).
With the current, smaller 2,744 B baseline there's room for roughly 14-15 more 256 B
threads (vs. ~8-10 estimated against the pre-trim baseline) before RAM becomes the
constraint — fine for a handful of concurrent tasks, but the fixed Zephyr baseline (not
per-thread cost) is what limits how far this scales on an 8 KB part.

## Further reading

See [`RUNBOOK.md`](RUNBOOK.md) for the full step-by-step procedure, the reasoning behind
each filter/flag, and a troubleshooting table covering every issue actually encountered
while building this (Windows file-lock quirks during `west init`, the `west packages pip
--install` trap, the 7-Zip-on-PATH requirement for SDK setup, and the pyOCD version bug
above).
