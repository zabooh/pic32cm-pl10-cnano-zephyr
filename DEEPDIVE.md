# PIC32CM PL10 Curiosity Nano — Deep dives

Longer *how it actually works* material, split out of [`README.md`](README.md) so that
page stays focused on getting the board built, flashed, and running. Everything here is
background/reference for this exact workspace and its pinned Zephyr revision — read the
README first if you just want to use the board.

For *why the workspace was built the way it was* (module filters, SDK setup, flashing
quirks), see [`RUNBOOK.md`](RUNBOOK.md).

## Contents

- [Peripheral support status](#peripheral-support-status)
- [Bridging a peripheral before Zephyr supports it](#bridging-a-peripheral-before-zephyr-supports-it)
- [Interrupts and the system tick](#interrupts-and-the-system-tick)
- [The driver model behind a pin toggle](#the-driver-model-behind-a-pin-toggle)
- [Idle, WFI, and CPU load](#idle-wfi-and-cpu-load)
- [Sleep modes, wake-up latency, and keeping state across a sleep](#sleep-modes-wake-up-latency-and-keeping-state-across-a-sleep)
- [PIC32CM PL10 device series](#pic32cm-pl10-device-series)
- [Porting to another PL10 device](#porting-to-another-pl10-device)

## Peripheral support status

How much of the PL10 does Zephyr actually drive today? This is a **snapshot of the pinned
Zephyr revision** (derived from the SoC devicetree plus the drivers present in the tree) —
it's exactly the kind of thing that moves forward when you
[update the pin](README.md#updating-the-zephyr-version-moving-the-pin). Two categories matter, and
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
what the [pin update](README.md#updating-the-zephyr-version-moving-the-pin) is for.

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
  read-only (see [Design decisions](README.md#design-decisions)). Editing the pinned tree fights the
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
([Updating the Zephyr version](README.md#updating-the-zephyr-version-moving-the-pin)) to a commit
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
5. Delete the "stopgap" notes in `CLAUDE.md` and this document.

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
land via a [pin update](README.md#updating-the-zephyr-version-moving-the-pin).

**Why this is the gap to watch.** Ultra-low-power is the PL10's headline strength: the whole
reason to pick this part for battery-powered, energy-harvesting, loop-powered, or
wake-on-event designs is its **~2 µA Standby mode with SleepWalking** — autonomous peripherals
(RTC, touch, ADC via the event system) that keep running and wake the core only on a real
event, no CPU in between. Under Zephyr today **none of that is reachable**: the best the stock
idle gives is the ~1.2 mA Idle tier, roughly **600× above** the part's advertised floor. So for
exactly the class of applications the PL10 is usually chosen for, a proper **Zephyr
power-management port — Standby plus SleepWalking wake sources — is arguably the single most
valuable piece of upstream support still missing**, more so than any individual peripheral
driver. Until it lands, a Zephyr-based PL10 design either implements Standby itself (as above)
or accepts mA-class idle and forfeits the low-power advantage that motivated the part.

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

## Sleep modes, wake-up latency, and keeping state across a sleep

A natural worry with deep sleep is: *if the chip powers most of itself down, does it have to
come back up through reset → C-runtime init → driver init → start the scheduler before it can
serve the application again — and is RAM gone, so I'd have to reconstruct the last state?*
For the PL10 the answer is reassuring, and it comes straight from the datasheet
(DS40002667): **the deepest sleep this silicon offers neither clears RAM nor resets the
CPU.** So most of that worry doesn't apply — but the parts of it that *do* apply (a genuine
reset, or a true power loss) are worth knowing exactly, so this section covers both.

### There are only two sleep modes, and both retain everything

`SLEEPCFG.SLEEPMODE` ([§14.6.1](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-73DF2464-590C-4D1A-BFA5-35334AE17955.html))
accepts exactly two values — `IDLE` (0x2) and `STANDBY` (0x4); every other code is
*reserved*. There is **no** "Off"/"Backup"/deep-sleep tier that wipes SRAM the way some other
low-power families have. You enter either mode by writing the field and executing `WFI`.

Both modes **retain the SRAM contents and the full logic state**
([§14.4.2.2](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-8C82C204-8FFA-4A79-8561-3DA739A87929.html)),
and waking from either **resumes program execution at the instruction after the `WFI`** —
the CPU takes the pending interrupt (or just runs on, depending on `PRIMASK`), it does *not*
vector to reset. Nothing re-runs: the Zephyr scheduler, every thread's stack, kernel timers,
and driver state are all exactly as they were the moment you slept.

| | Idle ([§14.4.4.1](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-726A8734-5E3E-47A9-B902-6537A1683408.html)) | Standby ([§14.4.4.2](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-9F979F28-27F5-4563-A353-B4CAB4DE3DCB.html)) |
|---|---|---|
| CPU | stopped | stopped |
| Clocks / peripherals | GCLK0 + MCLK stay live; AHB/APB gated unless a peripheral requests them | all stopped unless a peripheral keeps its own clock alive (**SleepWalking**) |
| SRAM + logic state | **retained** | **retained** (optionally *back-biased* for lower leakage — a retained-but-not-freely-accessible state; e.g. the DMAC can't reach back-biased RAM, [§19.4.4](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-E75FDBBE-4747-4F9C-BE62-A4D1AE4C5B4C.html)) |
| Regulator | main | ULP (or main — see the wake trade-off) |
| Typical current @ 24 MHz | **1.2 mA** ([Table 37-7](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-D4489DC3-1212-49E1-98F8-A8DFAB8A5D61.html)) | **2.0 µA** ([Table 37-8](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-5D2F58F6-00E1-4CE4-8EE2-A58643CF63E7.html)) |
| Wake-up latency | fastest (a few cycles) — GCLK0/MCLK never stopped | **24–120 µs** ([Table 37-10](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-2CE6F43E-C56B-435D-8192-75DB547C378B.html)) |
| On wake | resume after `WFI` | resume after `WFI` |

So "how fast can I serve the app again after deep sleep?" is **not** a reboot-time question on
this part — it's a wake-up-latency question, measured in microseconds, with RAM already intact.

### Wake-up latency, and the one knob that trades current for speed

The Idle tier keeps GCLK0 + MCLK running, so its wake is essentially immediate (the datasheet
calls it "the fastest wake-up time") — but it only buys you the 1.2 mA tier. Standby is where
the µA numbers live, and its wake latency has a single, explicit knob:
**`SUPC.VREG.RUNSTDBY`** ([Table 37-10](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-2CE6F43E-C56B-435D-8192-75DB547C378B.html)):

| `SUPC.VREG.RUNSTDBY` | Standby current | Wake from Standby |
|---|---|---|
| **0** — ULP regulator (default low-power) | **2.0 µA** | **120 µs** |
| **1** — main regulator kept powered in Standby | **190 µA** | **24 µs** |

That's the whole trade: keeping the main regulator alive through Standby costs ~188 µA extra
but cuts the wake latency ~5× (120 µs → 24 µs). For something that wakes rarely and can
tolerate a tenth of a millisecond, leave it at the 2 µA default; for something that wakes
often and must respond fast, `RUNSTDBY = 1` is the lever — you spend standby current to buy
responsiveness. (The 24/120 µs figures already include regulator settling; a clock source left
*on-demand* restarts within that window when the CPU or a peripheral requests it, so you don't
pay a separate OSCHF-restart on top.)

Two practical notes when you actually program this: there is a bus-bridge latency between the
store to `SLEEPCFG` and it taking effect, so **read the register back and confirm it holds the
intended value before `WFI`** ([§14.6.1](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-73DF2464-590C-4D1A-BFA5-35334AE17955.html)).
And a Standby design needs a wake source that stays alive in Standby — typically the **RTC**
(it keeps counting and can wake on a period) or a **pin edge via the EIC**; note the EIC has no
Zephyr node in this revision (see [Peripheral support status](#peripheral-support-status)),
which is the same reason the SW0 button polls today.

### Why you should sleep, not reset — the µs-vs-ms gap

The only thing on the PL10 that *does* run the full reset → startup → scheduler sequence is an
actual **reset**. It's worth seeing how much slower that path is, because it's exactly the path
Standby lets you avoid:

- **Standby wake:** 24–120 µs, then the next instruction runs. No re-init of anything.
- **A real reset:** the Boot ROM first brings OSCHF up at only **4 MHz** (OSCHF ÷ 6) and
  disables every other clock generator and the 32 K sources
  ([§9.3](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-67C2EA12-0FB2-4A90-8DEE-EE83C39180C2.html)),
  then Zephyr's startup re-runs the clock tree up to 24 MHz, the C runtime (`.data`/`.bss`),
  every driver's init, and finally creates the threads and starts the scheduler — **milliseconds**,
  from scratch.

That's a ~1000× difference. The takeaway for a low-power PL10 design is blunt: **to save power
you sleep in Standby; you never reset to save power.** Standby already gives you both the µA
floor *and* an in-place, RAM-intact resume — there is nothing faster to "reconstruct," because
nothing was torn down.

### When RAM really *is* gone: a true reset or power loss

RAM is only lost when the device actually resets or loses VDD — POR, brown-out (BODVDD), an
external `RESET`, a Watchdog reset, or a `SYSRESETREQ`/lockup. There is **no separate
always-powered backup-SRAM domain** on the PL10 (consistent with there being no Backup sleep
mode) — all SRAM is volatile and shares the fate of VDD. On the PL10, note that **all "user"
reset sources are treated like power resets**
([§4.6](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-6FB0EBA8-AACE-4E92-8900-4AEEBE02D34A.html)),
so don't assume a soft or Watchdog reset preserves more than a POR would.

If you genuinely need to survive one of those and reconstruct the last state, the two
mechanisms are:

1. **Read `RCAUSE` at the top of boot** to learn *why* you reset (POR / BODVDD / EXT / WDT /
   SYST / LOCKUP) and branch to a warm-start path
   ([§16.6.2](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-95B12DCB-8F8C-4F2D-94F3-846150B30031.html),
   [§16.4.2.2](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-DBF54462-8F0A-4176-A1D3-AACE383AB29D.html)).
2. **Checkpoint the state you care about into Flash (NVM)** before the risky window and reload
   it on boot. Mind the flash-corruption rules: don't start a Flash write when VDD is near the
   brown-out threshold, and use the BOD to hold the device in reset below it
   ([§26.4.2.4](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-58142CF0-442E-438E-BB1E-18A39E7C1DD4.html)).

For timekeeping specifically, the **RTC keeps counting through Standby** (and can be the wake
source), so wall-clock time is preserved across sleep for free; across a true power loss it
needs a backup supply.

### What this means for a Zephyr PM port

Today there's no PL10 PM port (see [Idle, WFI, and CPU load](#idle-wfi-and-cpu-load)), so
Zephyr's idle only ever executes a plain `WFI` in the **Idle** tier — it never programs
`SLEEPCFG = STANDBY`. That already gives you RAM-intact, near-instant resume trivially; it just
sits at 1.2 mA instead of 2 µA.

Reaching Standby is the same "bridge it in your own code" pattern as the ADC
([Bridging a peripheral…](#bridging-a-peripheral-before-zephyr-supports-it)), only closer to the
SoC: arm a wake source (RTC period, or an EIC pin once that lands), set `SUPC.VREG.RUNSTDBY`
for your latency/current target, write `SLEEPCFG.SLEEPMODE = STANDBY`, read it back, then `WFI`.
The encouraging part for such a port is exactly the finding above: because Standby resumes
in-place with SRAM retained, it behaves like **suspend-to-RAM that the CPU never notices** —
there is no checkpoint to save and no state to reconstruct. The real work of a PL10 Standby port
is therefore *choosing and arming wake sources* and *re-syncing the kernel tick* for the elapsed
sleep time — not rebuilding application state on the far side of a reset.

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
| **32 KB (3204)** | **High** | No Zephyr SoC support at all in this revision (no dtsi) — you'd add it. And the blocker: **4 KB SRAM won't hold the current 5.2 KB build** — you'd first trim threads/stacks/introspection (see [Memory usage](README.md#memory-usage)). |

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
