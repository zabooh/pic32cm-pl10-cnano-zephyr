/*
 * standby.c - "standby <seconds>" console command: put the SoC into PM Standby
 * sleep (~2 uA) and wake it after N seconds via the RTC.
 *
 * This is a bare-metal, direct-register bridge (same pattern as pl10_adc.c):
 * mainline Zephyr has no PM / power-domain support for the PIC32CM PL10, so
 * there is no pm_state / policy path to hook. We drive PM.SLEEPCFG + the RTC
 * directly, modelled on Microchip's Harmony reference app pm/pm_wakeup_rtc
 * (csp_apps_pic32cm_pl10 v1.0.0) and cross-checked against the datasheet
 * (DS40002667A 14.4.4.2 Standby, 21.4.4 RTC sleep operation).
 *
 * RTC clock: the internal 32 kHz RC is always available (its OSC32KCTRL control
 * has no enable bit), so OSC32KCTRL.RTCCTRL just selects its 1.024 kHz tap
 * (OSC1K) as the RTC source. With PRESCALER = DIV1 the 32-bit counter runs at
 * 1024 Hz, i.e. 1024 counts = 1 s (RTC_TICKS_PER_SEC). The RTC keeps running in
 * Standby and is an asynchronous wake source, so its compare match (CMP0) is
 * what pulls the CPU back out.
 *
 * Limitation: with no Zephyr PM integration we stop SysTick around __WFI(), so
 * the kernel tick (and every k_sleep-based thread) is frozen for the standby
 * duration - Zephyr uptime therefore lags real time by N seconds afterwards.
 * SRAM is retained in Standby, so we resume in place (no reset). Both are
 * cosmetic for a demo; a real PM port (tickless idle + sys_clock_announce())
 * is what would close the tick-drift gap. See DEEPDIVE.md -> Sleep modes.
 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <stdlib.h>

/* Pulls in core_cm0plus.h too: __WFI(), __DSB(), SysTick, NVIC_*. */
#include <pic32cm6408pl10048.h>

#include "cmd.h"
#include "led_ctrl.h"

#define RTC_TICKS_PER_SEC 1024U /* OSC1K (1.024 kHz) / PRESCALER DIV1 */

/* GCLK peripheral-channel index of the console SERCOM1 core clock. Fixed by
 * silicon (verified against Harmony's plib_clock.c: GCLK_PCHCTRL[8] =
 * SERCOM1_CORE, fed from GCLK0). The enabled UART keeps requesting this channel
 * (RX always listening), which holds GCLK0 -> OSCHF alive; disabling it for the
 * sleep lets OSCHF actually stop. */
#define GCLK_ID_SERCOM1_CORE 8U

/* Cap the nap so the board stays reachable: while in Standby the SWD port is
 * unreachable (worse than the WFI-idle case in CLAUDE.md), and the core only
 * comes back on the RTC match. It self-recovers to the prompt afterwards; a
 * stuck one still needs `pyocd reset` + `west flash`. */
#define STANDBY_MAX_SEC 3600U

/* --- RTC (MODE0, 32-bit counter) as the Standby wake source ------------- */

static void rtc_sync(uint32_t busy_mask)
{
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & busy_mask) {
    }
}

/* Tiny ISR - just clear the compare flag. It runs exactly once, right after we
 * irq_unlock() following wake-up (the IRQ is held pending under irq_lock). */
static void rtc_isr(const void *arg)
{
    ARG_UNUSED(arg);
    RTC_REGS->MODE0.RTC_INTFLAG = RTC_MODE0_INTFLAG_Msk;
}

static bool rtc_ready;

static void standby_rtc_init(void)
{
    /* RTC APB clock is on at reset (MCLK.APBAMASK reset value = 0x7FF); set it
     * defensively (|=, like pl10_adc.c) so we don't depend on that. */
    MCLK_REGS->MCLK_APBAMASK |= MCLK_APBAMASK_RTC_Msk;

    /* Internal 32 kHz RC needs no enable; route its 1.024 kHz tap to the RTC. */
    OSC32KCTRL_REGS->OSC32KCTRL_RTCCTRL = OSC32KCTRL_RTCCTRL_RTCSEL_OSC1K;

    /* Reset, then MODE0 (32-bit counter) with PRESCALER = DIV1. */
    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_SWRST_Msk;
    rtc_sync(RTC_MODE0_SYNCBUSY_SWRST_Msk);

    RTC_REGS->MODE0.RTC_CTRLA =
        RTC_MODE0_CTRLA_MODE_COUNT32 | RTC_MODE0_CTRLA_PRESCALER_DIV1;

    IRQ_CONNECT(RTC_IRQn, 0, rtc_isr, NULL, 0);
    irq_enable(RTC_IRQn);

    rtc_ready = true;
}

/* Arm a one-shot compare match `seconds` from now and start the counter. No
 * MATCHCLR, so it fires once; we stop the RTC right after wake-up. */
static void rtc_arm(uint32_t seconds)
{
    RTC_REGS->MODE0.RTC_COUNT = 0;
    rtc_sync(RTC_MODE0_SYNCBUSY_COUNT_Msk);

    RTC_REGS->MODE0.RTC_COMP = seconds * RTC_TICKS_PER_SEC;
    rtc_sync(RTC_MODE0_SYNCBUSY_COMP0_Msk);

    RTC_REGS->MODE0.RTC_INTFLAG = RTC_MODE0_INTFLAG_Msk; /* clear any stale flag */
    RTC_REGS->MODE0.RTC_INTENSET = RTC_MODE0_INTENSET_CMP0_Msk;

    RTC_REGS->MODE0.RTC_CTRLA |= RTC_MODE0_CTRLA_ENABLE_Msk;
    rtc_sync(RTC_MODE0_SYNCBUSY_ENABLE_Msk);
}

static void rtc_disarm(void)
{
    RTC_REGS->MODE0.RTC_INTENCLR = RTC_MODE0_INTENSET_CMP0_Msk;
    RTC_REGS->MODE0.RTC_CTRLA &= (uint16_t)~RTC_MODE0_CTRLA_ENABLE_Msk;
    rtc_sync(RTC_MODE0_SYNCBUSY_ENABLE_Msk);
}

/* --- Standby entry ------------------------------------------------------ */

static void enter_standby(uint32_t seconds)
{
    unsigned int key;
    uint32_t systick_ctrl;
    uint32_t apbc_mask;
    uint32_t sercom1_gclk;
    uint32_t gclk0_genctrl;
    uint32_t oschfctrl;

    rtc_arm(seconds);

    /* Critical section: from here to wake-up nothing else may run, and only the
     * RTC may pull us out. PRIMASK (irq_lock) does NOT block WFI wake-up on
     * Cortex-M0+, so the RTC IRQ still wakes the core - it just defers rtc_isr()
     * until irq_unlock(). Every other interrupt source (SERCOM console, ...) has
     * its clock gated in Standby and cannot wake us. */
    key = irq_lock();

    /* Stop SysTick, else the next kernel tick wakes us immediately. Save/restore
     * the whole control register so Zephyr's timer-driver state is untouched. */
    systick_ctrl = SysTick->CTRL;
    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;

    /* Gate the APBC bus (console SERCOM + ADC0) for the duration of the sleep.
     * The datasheet's ~2 uA Standby figure is measured with all APB clocks masked
     * except MCLK/NVMCTRL (errata Table 3-8, Note 1); the enabled console SERCOM
     * otherwise keeps requesting its clock, which holds OSCHF/GCLK0 alive and pins
     * the core at the ~1.2 mA Idle tier instead of letting it drop to Standby.
     * RTC (the wake source) lives on APBA and is untouched. Restored on wake so
     * the console comes back. NOTE: on the bare Curiosity Nano the on-board nEDBG
     * debugger and board rails dominate the USB-measured current and mask this;
     * the drop is only visible on the isolated target VTG rail. */
    apbc_mask = MCLK_REGS->MCLK_APBCMASK;
    MCLK_REGS->MCLK_APBCMASK = 0U;

    /* Drop the console SERCOM1 core-clock request too: gating only its APB clock
     * (above) still leaves the UART requesting its GCLK0 core clock, which keeps
     * GCLK0/OSCHF spinning. With both gone (and the CPU clock stopped by WFI) no
     * one requests GCLK0, so OSCHF stops and the core current falls toward the
     * Standby floor. Restored on wake so the console comes back. */
    sercom1_gclk = GCLK_REGS->GCLK_PCHCTRL[GCLK_ID_SERCOM1_CORE];
    GCLK_REGS->GCLK_PCHCTRL[GCLK_ID_SERCOM1_CORE] = sercom1_gclk & ~GCLK_PCHCTRL_CHEN_Msk;

    /* The real OSCHF keeper: the board devicetree configures GCLK0 (CPU/main
     * generator, sourced by OSCHF) with gclkgen-run-in-standby-en = 1, i.e.
     * GENCTRL[0].RUNSTDBY = 1. Per datasheet Table 14-3 a GCLK generator with
     * RUNSTDBY = 1 keeps running in Standby *unconditionally*, holding OSCHF
     * alive no matter what peripherals are gated - which is why gating the
     * SERCOM alone changed nothing. Clear it for the sleep so GCLK0 (and OSCHF)
     * can stop once nothing requests them; the CPU re-requests GCLK0 on wake. */
    gclk0_genctrl = GCLK_REGS->GCLK_GENCTRL[0];
    GCLK_REGS->GCLK_GENCTRL[0] = gclk0_genctrl & ~GCLK_GENCTRL_RUNSTDBY_Msk;
    while (GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL0_Msk) {
    }

    /* OSCHF (the 24 MHz core clock source) has no RUNSTDBY bit - only ONDEMAND.
     * With ONDEMAND = 0 (Zephyr default) it free-runs whenever enabled, so it
     * keeps drawing even in Standby regardless of who requests it. Switch it to
     * on-demand so it only runs while requested; with GCLK0 no longer running
     * and the CPU stopped, nothing requests it and it powers down. Restored on
     * wake (the CPU re-requests it; ~24 us OSCHF start-up). */
    oschfctrl = OSCCTRL_REGS->OSCCTRL_OSCHFCTRL;
    OSCCTRL_REGS->OSCCTRL_OSCHFCTRL = oschfctrl | OSCCTRL_OSCHFCTRL_ONDEMAND_Msk;

    /* Select Standby and confirm the write landed before WFI - the SLEEPCFG
     * write has bus-bridge latency (datasheet 14.6.1). */
    PM_REGS->PM_SLEEPCFG = PM_SLEEPCFG_SLEEPMODE_STANDBY;
    while ((PM_REGS->PM_SLEEPCFG & PM_SLEEPCFG_SLEEPMODE_Msk) !=
           PM_SLEEPCFG_SLEEPMODE_STANDBY) {
    }

    /* The datasheet's PM chapter describes only the SoC side (SLEEPCFG + WFI),
     * but the Cortex-M0+ core also has to signal *deep* sleep, not plain sleep,
     * or the PM never actually gates the clocks: WFI with SCR.SLEEPDEEP = 0
     * enters the shallow (Idle-equivalent, ~1.2 mA) state regardless of
     * SLEEPCFG. Zephyr's default idle leaves SLEEPDEEP clear, so we must set it
     * here to reach the ~2 uA Standby floor; restore it on wake so the kernel's
     * own idle stays shallow/fast. */
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

    __DSB();
    __WFI();
    /* ---- woke here on the RTC compare match ---- */

    OSCCTRL_REGS->OSCCTRL_OSCHFCTRL = oschfctrl; /* OSCHF ONDEMAND back */
    GCLK_REGS->GCLK_GENCTRL[0] = gclk0_genctrl;  /* GCLK0 RUNSTDBY back */
    while (GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL0_Msk) {
    }
    GCLK_REGS->GCLK_PCHCTRL[GCLK_ID_SERCOM1_CORE] = sercom1_gclk; /* SERCOM core clk back */
    MCLK_REGS->MCLK_APBCMASK = apbc_mask;                         /* SERCOM APB clk back */
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
    SysTick->CTRL = systick_ctrl;
    irq_unlock(key); /* pending RTC ISR now runs and clears the flag */

    rtc_disarm();
}

/* --- console command ---------------------------------------------------- */

static void standby_cmd(int argc, char **argv)
{
    if (argc != 2) {
        printk("usage: standby <seconds> (1-%u)\n", STANDBY_MAX_SEC);
        return;
    }

    int seconds = atoi(argv[1]);

    if (seconds < 1 || seconds > (int)STANDBY_MAX_SEC) {
        printk("standby: seconds must be 1-%u\n", STANDBY_MAX_SEC);
        return;
    }

    if (!rtc_ready) {
        standby_rtc_init();
    }

    printk("standby: entering Standby (~2 uA), RTC wake in %d s...\n", seconds);
    led_ctrl_off(); /* LED off so the low-power state is real (and visible) */

    enter_standby((uint32_t)seconds);

    printk("standby: woke after %d s (RTC compare match)\n", seconds);
    led_ctrl_blink(500); /* resume the default heartbeat */
}
CMD_REGISTER(standby, "standby", standby_cmd,
         "standby <s>     - PM Standby (~2uA) for <s> seconds, RTC wake");
