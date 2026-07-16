# PM Standby on the PIC32CM PL10 — how the `standby` command works, and why it reboots on wake

This document explains the `standby <s>` console command in depth: what it does,
the design journey behind it, why it takes the shape it does, and what would let
it be done "properly" in the future. It complements the short reference in
`README.md` and the essays in `DEEPDIVE.md`.

If you only want the summary: **`standby <s>` drops the SoC into real ~2 µA
Standby, waits ~`<s>` seconds on the SoC's own low-power timer, and then reboots
the board.** It is a self-contained, direct-register stopgap because mainline
Zephyr has no power-management driver for this SoC yet.

---

## 1. What it does

```
pl10:~$ standby 5
standby: Standby (~2 uA) for 5 s, then reboot...
*** Booting Zephyr OS build v4.4.0-... ***
PIC32CM PL10 Blinky - built ...
pl10:~$
```

- The LED goes dark, the console goes quiet, and the whole board current drops to
  the reset-level floor (see [§6 Measured results](#6-measured-results)).
- After roughly `<s>` seconds the SoC wakes itself and performs a **clean cold
  reboot** (`sys_reboot()`), so you see the boot banner and land back at the
  prompt with the blinky running again.
- Range: `1`–`3600` s. The nap is capped so a board is never unreachable for long
  (SWD cannot attach while the core is in Standby).

The reboot is deliberate — it is the price of making Standby reliable on a SoC
Zephyr doesn't support yet. The rest of this document is why.

---

## 2. The goal, and the honest outcome

The goal was the flagship "missing PL10 feature": a **Zephyr-conformant PM
Standby** where the SoC sleeps at microamp level during idle and the **scheduler
resumes in place** on wake — uptime, `k_sleep`, threads, all continuing as if
nothing happened.

The honest outcome: **resume-in-place is not achievable on the PL10 with the
current mainline Zephyr, because there is no PM/power-domain driver for this
SoC.** We built the whole Zephyr-PM integration ourselves and drove it into a
hardware wall (§4). Rather than ship a Standby that silently corrupts the
scheduler, the command reboots on wake (§5), which is 100 % reliable.

This is not a Zephyr limitation in general — it is specifically that **the PL10
device is not yet supported by an upstream PM driver.** When Microchip lands one,
the reboot stopgap can be replaced with a real resume-in-place path (§7).

---

## 3. Background: why Standby needs help from the timer

Zephyr is *tickless*: the kernel programs the system timer (here the Cortex-M
**SysTick**) to fire at the next scheduled deadline, and otherwise the CPU sits in
`WFI`. SysTick is clocked from the **CPU clock**. In PM Standby the CPU clock
stops — so **SysTick stops**, and the kernel loses its sense of time across the
nap.

Zephyr's answer is a *low-power companion*: a second timer that keeps running in
the low-power state and, on wake, tells the kernel how long it was out, so the
kernel clock and every `k_sleep`/`k_timer` stay correct. This is exactly the
mechanism a real SoC PM driver would wire up. On the PL10 there is no such driver,
so we tried to supply it.

---

## 4. The resume-in-place attempt, and the wall it hit

We implemented the full Zephyr-PM path:

- `CONFIG_PM=y` plus a devicetree `standby` power-state, and our own
  `pm_state_set()` / `pm_state_exit_post_ops()` (normally SoC code) that ran the
  clock-tree quiesce and `WFI`.
- A low-power system-timer companion (`CONFIG_SYSTEM_TIMER_LPM_COMPANION_HOOKS`)
  backed by the **RTC** (free-running 32-bit counter on the always-on internal
  32 kHz RC) and the **Watchdog** (WDT), with `CONFIG_SYSTEM_TIMER_RESET_BY_LPM`
  so the driver takes the elapsed time solely from the companion.

Along the way we solved several real problems and confirmed them on the bench:

- **Reaching the ~2 µA floor** needed an explicit clock-tree quiesce, not just
  `SLEEPCFG=STANDBY` + `WFI`: gate the console SERCOM's APB **and** its GCLK
  channel, clear GCLK0's run-in-standby bit (the board devicetree sets it, which
  otherwise held OSCHF alive unconditionally), and make OSCHF on-demand.
- **The RTC compare alone does not reliably wake** the core once OSCHF is gated
  off — its 32 kHz clock becomes unreliable. The **WDT's own always-on 32 kHz is
  rock-solid**, so the WDT Early-Warning interrupt is the dependable wake.
- With the companion reporting the WDT period as the elapsed time, and
  `RESET_BY_LPM` restarting SysTick fresh on wake, timing was correct: the kernel
  reported the requested nap length to the millisecond.

**But the scheduler still died after every nap.** A live blink-toggle counter
(the `led count` command) exposed it clearly: before a Standby the counter
climbs; after, it freezes. A tiny `k_uptime` probe made the mechanism undeniable:

| | uptime advance over ~2 s wall-clock | RTC counter |
| --- | --- | --- |
| before Standby | ~2400 ms (ticking normally) | running |
| after Standby | **~5 ms (tick dead)** | **still running** |

So the RTC *counter* survives the nap, but the **RTC compare interrupt stops
firing** afterwards — and an SWRST of the RTC does not bring it back. The tickless
kernel's low-power path relies on that compare interrupt to wake the short idles
between `k_sleep`s; with it dead, the kernel tick never advances, so every
`k_sleep`-based thread (blink, ADC stream, button poll) freezes, and the next
`standby` — which itself calls `k_sleep` — hangs outright.

We isolated the trigger by bisection:

- It happens after a **single** Standby cycle (not just the multi-step naps).
- It happens **with or without** `RESET_BY_LPM`, and after an RTC re-init.
- It happens even at the shallower **Idle tier** (~1.2 mA, OSCHF left running) —
  so it is the Zephyr-PM low-power *integration* on this unsupported SoC that
  breaks the tick, not one specific clock trick.

Conclusion: **gating the clock tree to reach Standby leaves the RTC compare path
in a state the kernel's low-power timer can't recover from, and there is no
upstream PL10 PM driver to do it correctly.** Resume-in-place is off the table
until that driver exists.

---

## 5. The shipped design: reboot on wake

Since resume-in-place can't be made reliable, the command is **self-contained**
(no `CONFIG_PM`, no companion, no dependence on the kernel surviving the nap) and
**reboots on wake**. All direct-register, modelled on Microchip's Harmony
`pm/pm_wakeup_rtc` example for `csp_apps_pic32cm_pl10` v1.0.0.

Sequence:

1. **LED off**, print the intent line.
2. Start the **RTC** as a free-running 32-bit counter on the internal 32 kHz RC
   (`OSC1K` tap). Its `COUNT` keeps running through Standby, so it is a reliable
   *elapsed-time reference* (the counter survives even though its compare doesn't).
3. **Stop SysTick** and `irq_lock()`, so only the WDT can wake the core. (On
   Cortex-M0+, `PRIMASK` does not block `WFI` wake-up, so the WDT still wakes us;
   we clear its flag in the loop instead of in an ISR.)
4. **Quiesce the clock tree** to reach the ~2 µA floor: gate the console SERCOM
   (APB + GCLK channel), clear GCLK0 run-in-standby, make OSCHF on-demand,
   `SLEEPCFG=STANDBY`, `SLEEPDEEP`. No restore is needed — we reboot.
5. **Nap loop:** arm the WDT Early-Warning for the nearest step that fits the
   remaining time, `WFI` (the SoC is at ~2 µA here), wake on the EW, clear its
   flag and pet the WDT (its reset period is a safety net), then re-read the RTC
   `COUNT`. Repeat until `COUNT` shows the target time has really elapsed.
6. **`sys_reboot(COLD)`** — a guaranteed-clean state on wake.

Because the RTC `COUNT` — not the WDT period — decides when the nap is done, a
WDT step firing a little late never accumulates drift, and naps of any length
(beyond a single WDT period) work by looping.

### Timing calibration

Nominally the `OSC1K` tap is 1.024 kHz, but with OSCHF gated off the underlying
ultra-low-power RC runs slow — **measured ~636 counts/s** on the bench, and very
stable/linear (uncalibrated: `standby 5` → 8.6 s, `standby 10` → 16.7 s wall).
The command times the nap against this calibrated rate, so `standby <s>` naps
~`<s>` real seconds. It is an RC oscillator, so the nap length is **approximate**
(drifts with board and temperature) — fine for a low-power demo, not a precision
timer.

---

## 6. Measured results

After calibration (wall-clock includes the ~0.6 s reboot/banner time):

| `standby <s>` | wall-clock to prompt |
| --- | --- |
| 2 | ~1.85 s |
| 3 | ~2.4 s |
| 5 | ~4.6 s |
| 10 | ~10.1 s |

Reliability: repeated and back-to-back naps always reboot cleanly — **no freeze,
no hang** — because each wake is a fresh boot by construction.

Current: the quiesce pulls the whole board down to the **same current as holding
the MCU in nRESET** (on this bench ~14 mA — the on-board nEDBG debugger + rails,
which firmware can't switch off). The target MCU's own ~2 µA is far below that
board-level floor and isn't separable on a board-level ammeter; to see the µA
itself you'd cut the Curiosity Nano Target-Power strap **J201** and meter the
target rail (User Guide §5.5). The mA-scale win *is* visible board-level
(reset → ~14 mA, idle → ~17 mA, `load on` → ~20 mA all resolve).

The `led count` command (a live blink-toggle counter) is kept in the firmware so
this can be checked interactively: it climbs while the blinky runs, confirming the
scheduler is healthy after each reboot.

---

## 7. Trade-offs and the path forward

**Trade-off:** the reboot loses uptime and program context across the nap. For a
low-power demo on an eval board this is a common and clean pattern — it still
demonstrates the core capability (the SoC drops to ~2 µA and wakes itself on its
own timer). It is simply not "the scheduler kept running."

**The path forward — a real Zephyr PM Standby with resume-in-place — depends on
upstream support:** when Microchip contributes a PM/power-domain driver for the
PL10 to Zephyr (the `pm_state_set()`/companion plumbing done in SoC code, with the
clock/RTC handling that keeps the low-power timer alive), this direct-register
reboot stopgap can be replaced with a proper resume-in-place path. That the PL10
still lacks this driver is precisely the "missing PL10 feature" this repository
set out to highlight.

---

## 8. Where the code lives

- `app/src/standby.c` — the entire command (RTC counter, WDT-EW wake loop, clock
  quiesce, `sys_reboot`). The file header carries a condensed version of this
  story.
- `app/prj.conf` — a note explaining why `CONFIG_PM` is intentionally absent.
- `app/src/led_ctrl.c` — the `led count` liveness counter used to verify the
  scheduler across naps.
