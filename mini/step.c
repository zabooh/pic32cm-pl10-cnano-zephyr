/*
 * step.c - bare-metal "step" demo for the current recording. NO Zephyr, minimal
 * init. Shows the MCU's whole controllable current range as clean steps.
 *
 * Sequence (each phase timed by the RTC on OSC32K, so both busy phases are real
 * ~3 s regardless of the CPU clock):
 *   1) ~3 s busy loop on OSCHF        (CPU active, boot-default ~4 MHz)
 *   2) switch OSCHF OFF               (ONDEMAND=1 + move GCLK0/CPU onto OSC32K)
 *   3) ~3 s busy loop on OSC32K       (CPU active at 32 kHz)
 *   4) Standby (WFI)                  (stays there until an external reset)
 *
 * Measured steps on this board (POWER-Z KM003C, USB), see mini/README.md:
 *   Phase 1 (OSCHF busy) : 16.20 mA
 *   Phase 2 (OSC32K busy): 15.33 mA   (-0.87: OSCHF off + CPU dynamic at 32 kHz)
 *   Standby              : 15.33 mA   (same as Phase 2 - 32 kHz CPU ~= WFI)
 * The ~15.3 mA floor is the on-board nEDBG debugger, not the MCU.
 *
 * OSCHF note: the SoC boots with OSCHFCTRL.ONDEMAND=0 (free-running), so OSCHF can
 * only be stopped after setting ONDEMAND=1 AND removing its requester (moving the
 * CPU/GCLK0 onto OSC32K). This demo does exactly that.
 */
#include <pic32cm6408pl10048.h>

#define RTC_HZ 32768u

__attribute__((always_inline)) static inline void gclk0_set_src(uint32_t src)
{
    uint32_t g = GCLK_REGS->GCLK_GENCTRL[0];
    g = (g & ~GCLK_GENCTRL_SRC_Msk) | GCLK_GENCTRL_SRC(src);
    GCLK_REGS->GCLK_GENCTRL[0] = g;
    while (GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL0_Msk) {
    }
}

void c_main(void)
{
    /* APB clocks we touch (boot-default should have them; set to be safe). */
    MCLK_REGS->MCLK_APBAMASK |= MCLK_APBAMASK_RTC_Msk | MCLK_APBAMASK_OSCCTRL_Msk |
                                MCLK_APBAMASK_OSC32KCTRL_Msk | MCLK_APBAMASK_GCLK_Msk;

    /* RTC MODE0 32-bit counter on OSC32K -> a stable 32.768 kHz time base
     * independent of the CPU clock; enabling it also starts OSC32K running. */
    OSC32KCTRL_REGS->OSC32KCTRL_RTCCTRL = OSC32KCTRL_RTCCTRL_RTCSEL_OSC32K;
    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_SWRST_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_SWRST_Msk) {
    }
    RTC_REGS->MODE0.RTC_CTRLA = RTC_MODE0_CTRLA_MODE_COUNT32 |
                                RTC_MODE0_CTRLA_PRESCALER_DIV1 |
                                RTC_MODE0_CTRLA_COUNTSYNC_Msk;
    RTC_REGS->MODE0.RTC_CTRLA |= RTC_MODE0_CTRLA_ENABLE_Msk;
    while (RTC_REGS->MODE0.RTC_SYNCBUSY & RTC_MODE0_SYNCBUSY_ENABLE_Msk) {
    }
    while (!(OSC32KCTRL_REGS->OSC32KCTRL_STATUS & OSC32KCTRL_STATUS_OSC32KRDY_Msk)) {
    }

    /* Phase 1: ~3 s busy on OSCHF. */
    while (RTC_REGS->MODE0.RTC_COUNT < 3u * RTC_HZ) {
    }

    /* Switch OSCHF OFF: on-demand, then move the CPU clock onto OSC32K so OSCHF
     * has no requester and stops. */
    OSCCTRL_REGS->OSCCTRL_OSCHFCTRL |= OSCCTRL_OSCHFCTRL_ONDEMAND_Msk;
    gclk0_set_src(GCLK_GENCTRL_SRC_OSC32K_Val);

    /* Phase 2: ~3 s busy on OSC32K (CPU now at 32 kHz). */
    while (RTC_REGS->MODE0.RTC_COUNT < 6u * RTC_HZ) {
    }

    /* Phase 3: Standby, no wake armed -> stays until an external reset. */
    PM_REGS->PM_SLEEPCFG = PM_SLEEPCFG_SLEEPMODE_STANDBY;
    while ((PM_REGS->PM_SLEEPCFG & PM_SLEEPCFG_SLEEPMODE_Msk) !=
           PM_SLEEPCFG_SLEEPMODE_STANDBY) {
    }
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    for (;;) {
        __DSB();
        __WFI();
    }
}
