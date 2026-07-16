/*
 * lpsleep.c - "lpsleep <s>": run-time deep low-power sleep that implements ALL of
 * the datasheet's ~2 uA Standby conditions, with resume in place (no reboot).
 *
 * THE JOURNEY (why this is the shape it is)
 * The shipped `standby` never drops below the ~15.8 mA debugger-halt level. We
 * chased why through a long measured investigation (see strommessung.md); the two
 * findings that matter:
 *
 * 1. ROOT CAUSE - OSCHF was never gateable. The SoC devicetree configures OSCHF
 *    at OSCHFCTRL.ONDEMAND=0 (free-running), so it runs forever no matter what -
 *    no sleep mode and no clock trick can stop it. Confirmed with the `oschftest`
 *    probe: OSCHFRDY stays 1 with no requester, and only flips to 0 once we set
 *    ONDEMAND=1. This is why standby/sbq/genoff all stuck at the same floor.
 *
 * 2. So lpsleep does the full recipe: set OSCHF ONDEMAND=1, move GCLK0 (the CPU
 *    clock) off OSCHF onto the internal 32.768 kHz OSC32K (independent of OSCHF and
 *    the RTC's clock), which lets OSCHF stop AND engages the ULP regulator
 *    (SUPC.VREG.RUNSTDBY=0 switches LDO->ULP when running on 32 kHz). It then
 *    enters STANDBY, masks all APB clocks except PM/MCLK/clock-controllers/RTC
 *    (AHBMASK is already 0x3FF), and disables every pin input buffer - i.e. every
 *    condition in datasheet Table 37-8. The free-running RTC compare (on OSC32K)
 *    wakes it; it switches GCLK0 back to OSCHF, restores everything, and continues.
 *
 * MEASURED OUTCOME (POWER-Z KM003C, USB rail): even with all of the above, the
 * board stays at ~15.83 mA - identical to standby. OSCHF turned out to be only
 * ~0.44 mA; the rest of the halt->reset-floor gap is the on-board nEDBG debugger,
 * a fixed board floor firmware cannot touch. Whether the *target* reaches the
 * datasheet ~2 uA can only be seen by metering the isolated target rail (cut J201);
 * from USB, ~15.8 mA is the wall. The target is now configured correctly for it.
 *
 * CAVEAT: interrupts are locked across the nap and the tickless SysTick is not
 * handed the elapsed time, so the Zephyr kernel tick does not advance - k_sleep
 * threads (blink) freeze on wake though the console stays alive. Experimental /
 * measurement command; a real resume-in-place PM driver would fix the tick.
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

/* Save every pin's config and disable its input buffer (INEN) - the datasheet
 * "I/Os in inactive input mode" condition; stops crowbar current on any floating
 * input. Fully restored on wake so the console/button/LED work again. */
static uint8_t pincfg_save[2][32];

static void io_quiesce(void)
{
    for (int g = 0; g < 2; g++) {
        for (int p = 0; p < 32; p++) {
            pincfg_save[g][p] = PORT_REGS->GROUP[g].PORT_PINCFG[p];
            PORT_REGS->GROUP[g].PORT_PINCFG[p] &= (uint8_t)~PORT_PINCFG_INEN_Msk;
        }
    }
}

static void io_restore(void)
{
    for (int g = 0; g < 2; g++) {
        for (int p = 0; p < 32; p++) {
            PORT_REGS->GROUP[g].PORT_PINCFG[p] = pincfg_save[g][p];
        }
    }
}

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

    /* Full-standby clock gating - the datasheet ~2 uA conditions (Table 37-8): mask
     * every APB clock except what we need to wake and resume (PM/MCLK, the clock
     * controllers used by the wake sequence, and the RTC). AHBMASK is already 0x3FF. */
    uint32_t apba_save = MCLK_REGS->MCLK_APBAMASK;
    uint32_t apbb_save = MCLK_REGS->MCLK_APBBMASK;
    uint32_t apbc_save = MCLK_REGS->MCLK_APBCMASK;
    MCLK_REGS->MCLK_APBAMASK = MCLK_APBAMASK_PM_Msk | MCLK_APBAMASK_MCLK_Msk |
                               MCLK_APBAMASK_OSCCTRL_Msk | MCLK_APBAMASK_OSC32KCTRL_Msk |
                               MCLK_APBAMASK_SUPC_Msk | MCLK_APBAMASK_GCLK_Msk |
                               MCLK_APBAMASK_RTC_Msk;
    MCLK_REGS->MCLK_APBBMASK = MCLK_APBBMASK_NVMCTRL_Msk;
    MCLK_REGS->MCLK_APBCMASK = 0U;

    io_quiesce(); /* disable all pin input buffers (last ~2 uA condition) */

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

    /* Restore the peripheral clocks and pin configs before touching anything else. */
    MCLK_REGS->MCLK_APBAMASK = apba_save;
    MCLK_REGS->MCLK_APBBMASK = apbb_save;
    MCLK_REGS->MCLK_APBCMASK = apbc_save;
    io_restore();

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
