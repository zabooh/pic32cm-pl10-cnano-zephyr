# Microchip Harmony 3 example apps for the PIC32CM PL10

A catalogue of the **bare-metal MPLAB Harmony 3 peripheral-library examples** that Microchip
ships for this exact device family, in the repo
[`csp_apps_pic32cm_pl10`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10)
(this list is from tag **v1.0.0**, the version this workspace cross-checks against). They are
**not** Zephyr — they use Harmony's CSP peripheral libraries (`plib_*`) and are opened in
MPLAB X / configured with MCC. They matter here for two reasons:

- They are the **register-level reference** for anything Zephyr doesn't drive on the PL10 yet.
  When you bridge a peripheral in `app/` (see
  [DEEPDIVE.md → Bridging a peripheral](DEEPDIVE.md#bridging-a-peripheral-before-zephyr-supports-it)),
  the matching Harmony example is the third independent source to cross-verify clock bits,
  pin/mux, and register sequences against — exactly how `pl10_adc.c` was validated.
- Several of them demonstrate the PL10's **headline low-power / SleepWalking** capability that
  Zephyr has no port for yet (see
  [DEEPDIVE.md → Sleep modes](DEEPDIVE.md#sleep-modes-wake-up-latency-and-keeping-state-across-a-sleep)).
  They are the working blueprint for a future Standby/PM bridge.

**Official documentation:** the per-example write-ups live in Microchip's online help —
[Code Examples for PIC32CM PL10](https://onlinedocs.microchip.com/v2/keyword-lookup?keyword=CSP_APPS_PIC32CM_PL10_INTRODUCTION&redirect=true)
(or download the web-help zip:
[GUID-FC69148D…](https://onlinedocs.microchip.com/download/GUID-FC69148D-5542-4F34-9236-530EA9E19336?type=webhelp)).
All app links below point at `v1.0.0`; the target board throughout is the
`pic32cm_pl10_curiosity_nano.X` project variant.

> Descriptions are derived from the repository structure at v1.0.0 and the standard,
> family-consistent Harmony CSP example semantics; the `ac_sleepwalk` entry was verified
> against its source directly. For the exact, officially-worded description of any single
> example, follow its online-docs page above.

**26 examples across 18 peripheral groups.** Grouped by theme:

## Low power & SleepWalking — the PL10's headline capability

Autonomous peripherals that keep working while the CPU sleeps in **Standby** (~2 µA) and wake
it only on a real event — no polling. This is the cluster most relevant to a battery/energy-
harvesting design and to a future Zephyr PM port.

| Example | Peripheral | What it demonstrates |
|---|---|---|
| [`ac/ac_sleepwalk`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/ac/ac_sleepwalk) | Analog Comparator | AC runs in Standby (`RUNSTDBY`) and wakes the CPU when an input pin crosses a scaled internal reference — autonomous analog-threshold detection with the core asleep. |
| [`adc/adc_window_sleepwalking`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/adc/adc_window_sleepwalking) | ADC | ADC monitors an input in Standby and wakes the CPU only when a sample falls inside/outside a configured window — autonomous analog window watch. |
| [`adc/adc_dmac_sleepwalking`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/adc/adc_dmac_sleepwalking) | ADC + DMAC + EVSYS | ADC samples autonomously in Standby and the DMAC moves results to RAM via the event system; the CPU wakes only after N samples — SleepWalking with data collection, zero CPU per sample. |
| [`pm/pm_wakeup_eic`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/pm/pm_wakeup_eic) | PM + EIC | Enter Standby, wake on an external pin edge (EIC) — the button-wake low-power pattern. |
| [`pm/pm_wakeup_rtc`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/pm/pm_wakeup_rtc) | PM + RTC | Enter Standby, wake periodically from the RTC — the timed-wake low-power pattern (usually the lowest-power periodic wake, no analog reference current). |

## Timers, counters & watchdog

| Example | Peripheral | What it demonstrates |
|---|---|---|
| [`systick/systick_periodic_timeout`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/systick/systick_periodic_timeout) | SysTick | The Cortex-M0+ SysTick used as a periodic timeout. |
| [`tc/tc_timer_mode`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/tc/tc_timer_mode) | TC | A Timer/Counter as a periodic timer raising an interrupt. |
| [`tc/tc_compare_mode`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/tc/tc_compare_mode) | TC | TC compare mode generating a timed output / waveform. |
| [`tc/tc_capture_mode`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/tc/tc_capture_mode) | TC | TC input capture — measure an external signal's period / pulse width. |
| [`tcc/tcc_synchronous_pwm_channels`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/tcc/tcc_synchronous_pwm_channels) | TCC | TCC generating multiple synchronized PWM channels (hardware PWM — the block Zephyr has no PL10 node for). |
| [`rtc/rtc_alarm`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/rtc/rtc_alarm) | RTC | RTC alarm interrupt at an absolute time. |
| [`rtc/rtc_periodic_timeout`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/rtc/rtc_periodic_timeout) | RTC | RTC periodic interrupt (free-running timed tick, survives Standby). |
| [`wdt/wdt_timeout`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/wdt/wdt_timeout) | WDT | Watchdog Timer resetting the device if the app fails to clear it in time. |

## Analog

| Example | Peripheral | What it demonstrates |
|---|---|---|
| [`adc/adc_sample`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/adc/adc_sample) | ADC | Basic ADC: convert an input channel and report the result — the plain (non-SleepWalking) counterpart to this workspace's stopgap [`pl10_adc.c`](app/src/pl10_adc.c). |

## Communication (SERCOM)

The PL10 has two SERCOM instances, each usable as UART / SPI / I²C.

| Example | Peripheral | What it demonstrates |
|---|---|---|
| [`sercom/usart`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/sercom/usart) | SERCOM USART | UART transmit/receive over the virtual COM port. |
| [`sercom/spi`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/sercom/spi) | SERCOM SPI | SPI master transfer to a peripheral device. |
| [`sercom/i2c`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/sercom/i2c) | SERCOM I²C | I²C master transfer (e.g. to an EEPROM / sensor). |

## DMA

| Example | Peripheral | What it demonstrates |
|---|---|---|
| [`dmac/dmac_memory_transfer`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/dmac/dmac_memory_transfer) | DMAC | Memory-to-memory block transfer driven entirely by the DMAC. |
| [`dmac/dmac_uart_echo`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/dmac/dmac_uart_echo) | DMAC + SERCOM | DMA-driven UART RX→TX echo with no CPU copy in the data path. |

## Clocks, reset, GPIO & flash

| Example | Peripheral | What it demonstrates |
|---|---|---|
| [`clock/clock_config`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/clock/clock_config) | GCLK / OSC | Configure the clock tree (oscillators, GCLK generators) and typically output a generated clock on a pin. |
| [`port/port_led_on_off_polling`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/port/port_led_on_off_polling) | PORT (GPIO) | Toggle an LED by *polling* a button — GPIO in/out, no interrupts. |
| [`eic/eic_led_on_off`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/eic/eic_led_on_off) | EIC | Toggle an LED from an *external interrupt* (EIC edge) — interrupt-driven GPIO. (The EIC is the block missing a Zephyr PL10 node, which is why this workspace's `button.c` polls.) |
| [`rstc/rstc_reset_cause`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/rstc/rstc_reset_cause) | RSTC | Read `RCAUSE` at boot and report *why* the device last reset (POR / BOD / WDT / external / system) — the warm-start hook described in [DEEPDIVE.md → When RAM really is gone](DEEPDIVE.md#sleep-modes-wake-up-latency-and-keeping-state-across-a-sleep). |
| [`nvmctrl/nvmctrl_flash_read_write`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/nvmctrl/nvmctrl_flash_read_write) | NVMCTRL | Self-program the on-chip flash: erase, write, and read back — the mechanism for persisting state across a true reset / power loss. |

## Event System & custom logic

| Example | Peripheral | What it demonstrates |
|---|---|---|
| [`evsys/evsys_trigger`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/evsys/evsys_trigger) | EVSYS | Route an event from one peripheral to another through the Event System, with no CPU in the path — the fabric that makes SleepWalking chains work. |
| [`ccl/manchester_encoder`](https://github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10/tree/v1.0.0/apps/ccl/manchester_encoder) | CCL | Use the Configurable Custom Logic to implement a Manchester encoder in hardware (glue logic without CPU or external gates). |

## How these map onto this Zephyr workspace

- **Ready in Zephyr today** (see [DEEPDIVE.md → Peripheral support status](DEEPDIVE.md#peripheral-support-status)):
  the `port`, `eic` (driver exists, node missing), `clock`, and `sercom` topics — GPIO, UART
  are active; SPI/I²C/DMA are a config step away.
- **Only reachable via the bridging pattern today:** everything analog and timing —
  `ac`, `adc`, `tc`, `tcc`, plus `evsys`/`ccl` and the whole low-power/SleepWalking cluster.
  For each of those, the Harmony example here is the register-level reference to bridge
  against, and the ADC one is exactly what [`pl10_adc.c`](app/src/pl10_adc.c) was modelled on.
- **The low-power gap:** `ac_sleepwalk`, `adc_*_sleepwalking`, and `pm_wakeup_*` together show
  the Standby + autonomous-wake behaviour that a Zephyr PL10 PM port would need to reproduce —
  the single most valuable missing piece of upstream support for this part.
