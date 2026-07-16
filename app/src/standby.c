/*
 * standby.c - "standby <s>" console command: real PM Standby (~2 uA) with a
 * clean reboot on wake.
 *
 * WHY REBOOT-ON-WAKE, NOT RESUME-IN-PLACE:
 * Mainline Zephyr has no PM/power-domain support for the PL10, so there is no
 * pm_state/companion path to hang a resume-in-place Standby off. We tried supplying
 * the hooks ourselves (CONFIG_PM + a low-power system-timer companion): the SoC
 * reached ~2 uA and woke reliably, but the kernel tick never recovered afterwards -
 * every k_sleep-based thread froze and a second Standby hung. Root cause: reaching
 * the Standby floor means gating OSCHF, which leaves the RTC's compare interrupt
 * dead; the tickless kernel's low-power path depends on exactly that interrupt to
 * wake short idles, so the tick dies. (This is the piece a real upstream PL10 PM
 * driver would provide.) Rather than ship a Standby that corrupts the scheduler,
 * this command is self-contained and REBOOTS on wake: guaranteed-clean state,
 * 100 % reliable, at the price of losing uptime/context across the nap. For a
 * low-power demo on an eval board that trade-off is fine - it still shows the SoC
 * dropping to ~2 uA and waking on its own timer.
 *
 * HOW IT WORKS (all direct-register, modelled on Microchip's Harmony
 * pm/pm_wakeup_rtc for csp_apps_pic32cm_pl10 v1.0.0):
 *   - RTC MODE0 32-bit counter on the always-on internal 32 kHz RC (OSC1K tap,
 *     1.024 kHz -> 1024 counts = 1 s). Its COUNT keeps running through Standby even
 *     with OSCHF off, so it is our reliable elapsed-time reference.
 *   - The Watchdog's Early-Warning interrupt is the reliable periodic wake (its own
 *     always-on 32 kHz survives OSCHF-off, where the RTC compare does not). We WFI
 *     in a loop, re-checking the RTC COUNT on each EW wake until <s> seconds have
 *     really elapsed, then sys_reboot(). The WDT's reset period is a safety net.
 *   - The OSCHF/GCLK0/SERCOM clock-tree quiesce (the part that actually reaches the
 *     ~2 uA floor) runs once before the loop; no restore is needed since we reboot.
 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/reboot.h>
#include <stdlib.h>

#include <pic32cm6408pl10048.h>

#include "cmd.h"
#include "led_ctrl.h"

/* GCLK peripheral-channel index of the console SERCOM1 core clock (silicon-fixed;
 * cross-checked against Harmony plib_clock.c). */
#define GCLK_ID_SERCOM1_CORE 8U

/* Effective RTC/WDT count rate DURING Standby. Nominally the OSC1K tap is
 * 1.024 kHz, but with OSCHF gated off the internal ultra-low-power 32 kHz RC that
 * ultimately clocks it runs slow - measured ~636 counts/s on this board (stable
 * and linear: standby 5 s -> 8.6 s, 10 s -> 16.7 s wall before calibration). We
 * time the nap by this rate so "standby <s>" naps ~<s> real seconds. It is an RC
 * oscillator, so this is approximate and drifts with board/temperature; the nap
 * length is best-effort, not precise (fine for a low-power demo). */
#define STANDBY_HZ 640U       /* calibrated effective count rate in Standby */
#define STANDBY_MAX_SEC 3600U /* cap: SWD is unreachable in Standby (self-recovers) */

/* WDT Early-Warning wake steps, largest first, with their period in RTC counts
 * (both the WDT and the RTC run at ~1.024 kHz, so 1 count ~= 1 WDT cycle). We pick
 * the largest step that still fits the remaining time; the RTC COUNT - not the WDT
 * period - decides when we are actually done, so a late-firing WDT never drifts
 * the total. */
static const struct {
    uint32_t off;    /* WDT_EWCTRL_EWOFFSET_CYCn_Val */
    uint32_t counts; /* nominal period in RTC counts */
} wdt_ew_steps[] = {
    {WDT_EWCTRL_EWOFFSET_CYC8192_Val, 8192U},
    {WDT_EWCTRL_EWOFFSET_CYC4096_Val, 4096U},
    {WDT_EWCTRL_EWOFFSET_CYC2048_Val, 2048U},
    {WDT_EWCTRL_EWOFFSET_CYC512_Val, 512U},
    {WDT_EWCTRL_EWOFFSET_CYC256_Val, 256U},
};

static void rtc_start(void)
{
    MCLK_REGS->MCLK_APBAMASK |= MCLK_APBAMASK_RTC_Msk;
    OSC32KCTRL_REGS->OSC32KCTRL_RTCCTRL = OSC32KCTRL_RTCCTRL_RTCSEL_OSC1K;

    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_SWRST_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_SWRST_Msk) {
    }
    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_MODE_COUNT32 |
                                RTC_MODE0_CTRLA_PRESCALER_DIV1 |
                                RTC_MODE0_CTRLA_COUNTSYNC_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_COUNTSYNC_Msk) {
    }
    RTC_REGS->MODE0.RTC_CTRLA |= RTC_MODE0_CTRLA_ENABLE_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_ENABLE_Msk) {
    }
}

static uint32_t rtc_count(void)
{
    return RTC_REGS->MODE0.RTC_COUNT;
}

/* Arm the WDT Early-Warning for a wake this many RTC counts from now (nearest
 * fitting step). The reset period is the longest available, a safety net in case
 * the wake loop ever stalls. */
static void wdt_arm_ew(uint32_t remaining_counts)
{
    size_t sel = ARRAY_SIZE(wdt_ew_steps) - 1U; /* smallest by default */

    for (size_t i = 0; i < ARRAY_SIZE(wdt_ew_steps); i++) {
        if (wdt_ew_steps[i].counts <= remaining_counts) {
            sel = i;
            break;
        }
    }

    WDT_REGS->WDT_CTRLA &= (uint8_t)~WDT_CTRLA_ENABLE_Msk;
    while (WDT_REGS->WDT_SYNCBUSY & WDT_SYNCBUSY_ENABLE_Msk) {
    }
    WDT_REGS->WDT_CONFIG = (uint8_t)WDT_CONFIG_PER(WDT_CONFIG_PER_CYC16384_Val);
    WDT_REGS->WDT_EWCTRL = (uint8_t)WDT_EWCTRL_EWOFFSET(wdt_ew_steps[sel].off);
    WDT_REGS->WDT_INTFLAG = WDT_INTFLAG_EW_Msk;
    WDT_REGS->WDT_INTENSET = WDT_INTENSET_EW_Msk;
    WDT_REGS->WDT_CTRLA |= WDT_CTRLA_ENABLE_Msk;
    while (WDT_REGS->WDT_SYNCBUSY & WDT_SYNCBUSY_ENABLE_Msk) {
    }
}

/* Quiesce the clock tree so OSCHF actually stops in Standby -> the ~2 uA floor.
 * No restore: the command reboots on wake. */
static void standby_quiesce(void)
{
    MCLK_REGS->MCLK_APBCMASK = 0U; /* gate the console SERCOM's APB clock */
    GCLK_REGS->GCLK_PCHCTRL[GCLK_ID_SERCOM1_CORE] &= ~GCLK_PCHCTRL_CHEN_Msk;

    /* GCLK0 (CPU generator) is run-in-standby by the board DT, which would hold
     * OSCHF alive unconditionally; clear it for the sleep. */
    GCLK_REGS->GCLK_GENCTRL[0] &= ~GCLK_GENCTRL_RUNSTDBY_Msk;
    while (GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL0_Msk) {
    }

    /* OSCHF has no run-in-standby bit; make it on-demand so it stops once GCLK0
     * stops requesting it in Standby (it restarts on-demand when the core wakes). */
    OSCCTRL_REGS->OSCCTRL_OSCHFCTRL |= OSCCTRL_OSCHFCTRL_ONDEMAND_Msk;

    PM_REGS->PM_SLEEPCFG = PM_SLEEPCFG_SLEEPMODE_STANDBY;
    while ((PM_REGS->PM_SLEEPCFG & PM_SLEEPCFG_SLEEPMODE_Msk) !=
           PM_SLEEPCFG_SLEEPMODE_STANDBY) {
    }
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
}

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

    printk("standby: Standby (~2 uA) for %d s, then reboot...\n", seconds);
    led_ctrl_off();

    uint32_t target = (uint32_t)seconds * STANDBY_HZ;

    rtc_start(); /* COUNT starts at 0 and runs through Standby */

    /* Only the WDT may wake us: stop SysTick (its tick would wake WFI early) and
     * lock interrupts. On Cortex-M0+ PRIMASK does not block WFI wake-up, so the
     * WDT still wakes the core; we clear its flag in the loop (no ISR needed). */
    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
    NVIC_ClearPendingIRQ(WDT_IRQn);
    NVIC_EnableIRQ(WDT_IRQn);
    unsigned int key = irq_lock();

    standby_quiesce();

    for (;;) {
        uint32_t now = rtc_count();

        if (now >= target) {
            break;
        }
        wdt_arm_ew(target - now);

        __DSB();
        __WFI();
        /* woke on the WDT Early-Warning (or its reset net as a backstop) */

        WDT_REGS->WDT_INTFLAG = WDT_INTFLAG_EW_Msk;
        WDT_REGS->WDT_CLEAR = WDT_CLEAR_CLEAR_KEY_Val; /* pet the reset net */
        NVIC_ClearPendingIRQ(WDT_IRQn);
    }

    irq_unlock(key);
    sys_reboot(SYS_REBOOT_COLD); /* clean state on wake */
}
CMD_REGISTER(standby, "standby", standby_cmd,
         "standby <s>     - PM Standby (~2uA) for <s> s, then reboot");
