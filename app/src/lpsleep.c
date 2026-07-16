/*
 * lpsleep.c - "lpsleep <s>": REAL low-power sleep with OSCHF actually switched
 * off, resume-in-place (no reboot).
 *
 * WHY THIS EXISTS
 * The shipped `standby` only reaches the debugger-halt level (~15.9 mA), never
 * the reset floor (~12 mA), because OSCHF keeps running the whole time: GCLK0 (the
 * CPU clock generator) is sourced from OSCHF, so it *requests* OSCHF continuously,
 * and OSCHF is an ONDEMAND-only oscillator (no ENABLE bit) - it can only stop when
 * nothing requests it. So the ~4 mA of OSCHF is never removed.
 *
 * THE FIX (what the user asked for)
 * Move the CPU clock off OSCHF before sleeping: switch GCLK0's source from OSCHF
 * to the internal 32.768 kHz OSC32K - the same oscillator that clocks the RTC, and
 * fully independent of OSCHF. Then:
 *   - OSCHF has no requester -> ONDEMAND stops it -> the ~4 mA drops.
 *   - The core keeps running, just at 32 kHz (never loses its clock, unlike the
 *     old CONFIG_PM path that gated OSCHF out from under a still-OSCHF core).
 *   - The RTC (on OSC32K) keeps counting and its compare interrupt wakes the core.
 *   - On wake the core is already running on OSC32K; we then switch GCLK0 back to
 *     OSCHF (writing the source re-requests OSCHF, which restarts) and continue.
 *
 * Datasheet: OSC32K is request-started, no ENABLE needed, ONDEMAND=1 by reset
 * (13.4.2.1 / 13.6.10); OSCHF is ONDEMAND-only, so removing all requesters stops
 * it. Plain WFI/IDLE is used (not STANDBY) - OSCHF-off is governed by the request,
 * not the sleep mode, and IDLE keeps the RTC running to guarantee the wake.
 *
 * CAVEAT: SysTick is stopped and interrupts are locked across the nap, so the
 * Zephyr kernel tick does not advance during it (k_uptime/timers lag by ~<s> s
 * afterwards). Fine for a low-power demo/measurement; a real resume-in-place PM
 * driver would hand the lost ticks back to the kernel.
 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <stdlib.h>

#include <pic32cm6408pl10048.h>

#include "cmd.h"
#include "led_ctrl.h"
#include "pl10_adc.h"

#define OSC32K_HZ 32768U       /* nominal OSC32K rate (RC, approximate) */
#define LPSLEEP_MAX_SEC 600U

static void gclk0_set_src(uint32_t src_val)
{
    uint32_t g = GCLK_REGS->GCLK_GENCTRL[0];
    g = (g & ~GCLK_GENCTRL_SRC_Msk) | GCLK_GENCTRL_SRC(src_val);
    GCLK_REGS->GCLK_GENCTRL[0] = g;
    while (GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL0_Msk) {
    }
}

/* RTC MODE0 32-bit counter on OSC32K (32.768 kHz), one-shot compare at `target`
 * counts. Enabling the RTC generates the OSC32K clock request that starts it. */
static void rtc_start_compare(uint32_t target)
{
    MCLK_REGS->MCLK_APBAMASK |= MCLK_APBAMASK_RTC_Msk;
    OSC32KCTRL_REGS->OSC32KCTRL_RTCCTRL = OSC32KCTRL_RTCCTRL_RTCSEL_OSC32K;

    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_SWRST_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_SWRST_Msk) {
    }

    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_MODE_COUNT32 | RTC_MODE0_CTRLA_PRESCALER_DIV1;

    RTC_REGS->MODE0.RTC_COMP = target;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_COMP0_Msk) {
    }

    RTC_REGS->MODE0.RTC_INTFLAG = RTC_MODE0_INTFLAG_CMP0_Msk;
    RTC_REGS->MODE0.RTC_INTENSET = RTC_MODE0_INTENSET_CMP0_Msk;

    RTC_REGS->MODE0.RTC_CTRLA |= RTC_MODE0_CTRLA_ENABLE_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_ENABLE_Msk) {
    }
}

static void rtc_stop(void)
{
    RTC_REGS->MODE0.RTC_INTENCLR = RTC_MODE0_INTENCLR_CMP0_Msk;
    RTC_REGS->MODE0.RTC_CTRLA &= (uint16_t)~RTC_MODE0_CTRLA_ENABLE_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_ENABLE_Msk) {
    }
    /* RTC disabled -> its OSC32K request drops -> OSC32K (ONDEMAND) stops. */
}

static void lpsleep_cmd(int argc, char **argv)
{
    if (argc != 2) {
        printk("usage: lpsleep <seconds> (1-%u)\n", LPSLEEP_MAX_SEC);
        return;
    }
    int seconds = atoi(argv[1]);
    if (seconds < 1 || seconds > (int)LPSLEEP_MAX_SEC) {
        printk("lpsleep: seconds must be 1-%u\n", LPSLEEP_MAX_SEC);
        return;
    }

    printk("lpsleep: core -> OSC32K, OSCHF off, RTC wake after ~%d s, then resume...\n",
           seconds);
    led_ctrl_off();
    pl10_adc_disable();
    k_msleep(50); /* let the console drain at full speed first */

    uint32_t target = (uint32_t)seconds * OSC32K_HZ;

    /* Lock interrupts so only our loop runs (on Cortex-M0+ PRIMASK does not block
     * WFI wake-up, so the RTC still wakes us; we poll its flag, no ISR). SysTick is
     * left running - we do NOT disable it, so the kernel's tick timer stays intact
     * across the nap (disabling it is what froze the scheduler on wake). */
    NVIC_ClearPendingIRQ(RTC_IRQn);
    NVIC_EnableIRQ(RTC_IRQn);
    unsigned int key = irq_lock();

    rtc_start_compare(target);
    while (!(OSC32KCTRL_REGS->OSC32KCTRL_STATUS & OSC32KCTRL_STATUS_OSC32KRDY_Msk)) {
    }

    /* CRITICAL: the boot/DT config leaves OSCHF at ONDEMAND=0 (free-running), so it
     * never stops even with no requester. Set ONDEMAND=1 so it becomes request-gated;
     * only then does removing GCLK0 below actually switch it off. */
    uint32_t oschfctrl_save = OSCCTRL_REGS->OSCCTRL_OSCHFCTRL;
    OSCCTRL_REGS->OSCCTRL_OSCHFCTRL = oschfctrl_save | OSCCTRL_OSCHFCTRL_ONDEMAND_Msk;

    /* Move the CPU off OSCHF -> OSCHF loses its last requester. Keep GCLK0 running
     * in standby (RUNSTDBY) so the core clock and the OSC32K request survive the
     * nap (the RTC, clocked by OSC32K, then keeps counting to wake us). */
    gclk0_set_src(GCLK_GENCTRL_SRC_OSC32K_Val);
    GCLK_REGS->GCLK_GENCTRL[0] |= GCLK_GENCTRL_RUNSTDBY_Msk;
    while (GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL0_Msk) {
    }

    /* Enter STANDBY sleep mode. This is the missing piece: only STANDBY actually
     * gates an oscillator that no longer has a requester, so this is what finally
     * stops OSCHF. IDLE alone leaves an unrequested OSCHF running. */
    PM_REGS->PM_SLEEPCFG = PM_SLEEPCFG_SLEEPMODE_STANDBY;
    while ((PM_REGS->PM_SLEEPCFG & PM_SLEEPCFG_SLEEPMODE_Msk) !=
           PM_SLEEPCFG_SLEEPMODE_STANDBY) {
    }
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

    for (;;) {
        if (RTC_REGS->MODE0.RTC_INTFLAG & RTC_MODE0_INTFLAG_CMP0_Msk) {
            break;
        }
        /* Clear a pending SysTick so it doesn't make WFI return immediately (its
         * ISR is masked by irq_lock); the RTC compare is the real wake. */
        SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
        __DSB();
        __WFI();
    }
    RTC_REGS->MODE0.RTC_INTFLAG = RTC_MODE0_INTFLAG_CMP0_Msk;

    /* Leave deep sleep. */
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
    PM_REGS->PM_SLEEPCFG = PM_SLEEPCFG_SLEEPMODE_IDLE;

    /* Back to full speed: re-request OSCHF and wait until it is ready, then drop
     * GCLK0's run-in-standby again. */
    gclk0_set_src(GCLK_GENCTRL_SRC_OSCHF_Val);
    while (!(OSCCTRL_REGS->OSCCTRL_STATUS & OSCCTRL_STATUS_OSCHFRDY_Msk)) {
    }
    GCLK_REGS->GCLK_GENCTRL[0] &= ~GCLK_GENCTRL_RUNSTDBY_Msk;
    while (GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL0_Msk) {
    }
    OSCCTRL_REGS->OSCCTRL_OSCHFCTRL = oschfctrl_save; /* restore original ONDEMAND state */

    rtc_stop();
    NVIC_DisableIRQ(RTC_IRQn);
    NVIC_ClearPendingIRQ(RTC_IRQn);
    irq_unlock(key);

    printk("lpsleep: resumed on OSCHF.\n");
}
CMD_REGISTER(lpsleep, "lpsleep", lpsleep_cmd,
         "lpsleep <s>     - OSCHF-off sleep on OSC32K, RTC wake, resume-in-place");

/*
 * Diagnostic: does OSCHF actually STOP when GCLK0 no longer sources it? Switch the
 * CPU to OSC32K (OSCHF loses its only requester), spin briefly at 32 kHz, sample
 * OSCCTRL.OSCHFRDY, then switch back and report. No sleep involved - this tests the
 * pure ONDEMAND gating in ACTIVE mode. OSCHFRDY==0 => OSCHF stopped as documented;
 * ==1 => it stays on despite no requester (a boot/latch effect).
 */
static void oschftest_cmd(int argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    k_msleep(30); /* drain console */

    unsigned int key = irq_lock();

    /* Bring OSC32K up by requesting it via the RTC (no interrupt needed here). */
    MCLK_REGS->MCLK_APBAMASK |= MCLK_APBAMASK_RTC_Msk;
    OSC32KCTRL_REGS->OSC32KCTRL_RTCCTRL = OSC32KCTRL_RTCCTRL_RTCSEL_OSC32K;
    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_SWRST_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_SWRST_Msk) {
    }
    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_MODE_COUNT32 | RTC_MODE0_CTRLA_PRESCALER_DIV1;
    RTC_REGS->MODE0.RTC_CTRLA |= RTC_MODE0_CTRLA_ENABLE_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_ENABLE_Msk) {
    }
    while (!(OSC32KCTRL_REGS->OSC32KCTRL_STATUS & OSC32KCTRL_STATUS_OSC32KRDY_Msk)) {
    }

    uint32_t rdy_before = (OSCCTRL_REGS->OSCCTRL_STATUS & OSCCTRL_STATUS_OSCHFRDY_Msk) ? 1U : 0U;
    uint32_t oschfctrl = OSCCTRL_REGS->OSCCTRL_OSCHFCTRL;

    /* CPU off OSCHF -> OSCHF now has no requester. */
    gclk0_set_src(GCLK_GENCTRL_SRC_OSC32K_Val);
    uint32_t gen0_after = GCLK_REGS->GCLK_GENCTRL[0]; /* confirm SRC really changed */
    for (volatile int i = 0; i < 3000; i++) {
        __NOP();
    }
    uint32_t rdy_unreq = (OSCCTRL_REGS->OSCCTRL_STATUS & OSCCTRL_STATUS_OSCHFRDY_Msk) ? 1U : 0U;

    /* Your idea: re-assert the OSCHF config (ONDEMAND) while it is unrequested, in
     * case the boot state latched it on and it needs re-claiming to become gateable. */
    OSCCTRL_REGS->OSCCTRL_OSCHFCTRL = oschfctrl | OSCCTRL_OSCHFCTRL_ONDEMAND_Msk;
    for (volatile int i = 0; i < 3000; i++) {
        __NOP();
    }
    uint32_t rdy_rearm = (OSCCTRL_REGS->OSCCTRL_STATUS & OSCCTRL_STATUS_OSCHFRDY_Msk) ? 1U : 0U;

    /* back to OSCHF */
    gclk0_set_src(GCLK_GENCTRL_SRC_OSCHF_Val);
    while (!(OSCCTRL_REGS->OSCCTRL_STATUS & OSCCTRL_STATUS_OSCHFRDY_Msk)) {
    }
    rtc_stop();
    irq_unlock(key);

    printk("oschftest: GENCTRL0 after switch=0x%08x (SRC=%u, 3=OSC32K)\n",
           gen0_after, (unsigned)(gen0_after & GCLK_GENCTRL_SRC_Msk));
    printk("oschftest: OSCHFCTRL=0x%08x  OSCHFRDY before=%u unreq=%u re-armed=%u  (0=stopped)\n",
           oschfctrl, rdy_before, rdy_unreq, rdy_rearm);
}
CMD_REGISTER(oschftest, "oschftest", oschftest_cmd,
         "oschftest       - does OSCHF stop when GCLK0 leaves it? (register probe)");
