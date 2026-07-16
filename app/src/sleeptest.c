/*
 * sleeptest.c - "stest <mode>" console command: a sleep-mode experiment matrix
 * for characterising the PL10's real low-power draw (POWER-Z current capture).
 *
 * WHY: the shipped "standby" command only reaches IDLE-level current
 * (~17.35 mA on this board), NOT deep STANDBY. This module sweeps sleep
 * configurations back-to-back so one continuous current capture can show which
 * knob actually drops the draw toward the reset floor.
 *
 * Modes (all direct-register, modelled on standby.c):
 *   idle     - PM=IDLE,    SLEEPDEEP=0, no clock gating       (idle reference)
 *   idledeep - PM=IDLE,    SLEEPDEEP=1, no clock gating       (deep bit, idle mode)
 *   sb0      - PM=STANDBY, SLEEPDEEP=1, no clock gating       (standby mode bits only)
 *   sbq      - PM=STANDBY, SLEEPDEEP=1, full clock quiesce    (== standby's quiesce, no wake IRQ)
 *   genoff   - sbq, then also stop GCLK0 (CPU generator) itself (most aggressive)
 *   off      - sbq, NO wake armed -> stuck until EXTERNAL reset (debugger)
 *
 * All modes except 'off' arm a WDT hard-reset (~8-13 s) as the only wake: the
 * core reboots on WDT timeout ("fixed WDT time" recovery). 'off' arms nothing
 * and stays asleep until a pyocd reset. SysTick is stopped and interrupts locked
 * so only the WDT (its own always-on 32 kHz; a reset is not maskable by PRIMASK)
 * can end the nap.
 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <string.h>

#include <pic32cm6408pl10048.h>

#include "cmd.h"
#include "led_ctrl.h"
#include "pl10_adc.h"

#define GCLK_ID_SERCOM1_CORE 8U

struct sleep_cfg {
    const char *name;
    bool sleepdeep;    /* SCB->SCR.SLEEPDEEP */
    uint32_t pm_mode;  /* PM_SLEEPCFG_SLEEPMODE_* value */
    uint8_t gate;      /* 0 = none, 1 = quiesce clock tree, 2 = quiesce + stop GCLK0 */
    bool wake_wdt;     /* true = arm WDT hard-reset; false = stuck until ext. reset */
};

static const struct sleep_cfg modes[] = {
    {"idle",     false, PM_SLEEPCFG_SLEEPMODE_IDLE_Val,    0U, true},
    {"idledeep", true,  PM_SLEEPCFG_SLEEPMODE_IDLE_Val,    0U, true},
    {"sb0",      true,  PM_SLEEPCFG_SLEEPMODE_STANDBY_Val, 0U, true},
    {"sbq",      true,  PM_SLEEPCFG_SLEEPMODE_STANDBY_Val, 1U, true},
    {"genoff",   true,  PM_SLEEPCFG_SLEEPMODE_STANDBY_Val, 2U, true},
    {"off",      true,  PM_SLEEPCFG_SLEEPMODE_STANDBY_Val, 1U, false},
};

/* Arm the Watchdog for a plain (non-windowed) reset after ~8192 clock cycles.
 * Do NOT pet it -> it resets the SoC when the period elapses. Clocked by the
 * always-on ULP 32 kHz, so it fires regardless of what we gate. */
static void wdt_arm_reset(void)
{
    WDT_REGS->WDT_CTRLA &= (uint8_t)~WDT_CTRLA_ENABLE_Msk;
    while (WDT_REGS->WDT_SYNCBUSY & WDT_SYNCBUSY_ENABLE_Msk) {
    }
    WDT_REGS->WDT_CONFIG = (uint8_t)WDT_CONFIG_PER(WDT_CONFIG_PER_CYC8192_Val);
    WDT_REGS->WDT_EWCTRL = 0U; /* no early-warning; plain reset only */
    WDT_REGS->WDT_CTRLA |= WDT_CTRLA_ENABLE_Msk;
    while (WDT_REGS->WDT_SYNCBUSY & WDT_SYNCBUSY_ENABLE_Msk) {
    }
}

static void quiesce_clock_tree(void)
{
    pl10_adc_disable(); /* remove ADC analog bias before its APB clock is gated */

    MCLK_REGS->MCLK_APBCMASK = 0U; /* gate console SERCOM1 APB */
    GCLK_REGS->GCLK_PCHCTRL[GCLK_ID_SERCOM1_CORE] &= ~GCLK_PCHCTRL_CHEN_Msk;

    GCLK_REGS->GCLK_GENCTRL[0] &= ~GCLK_GENCTRL_RUNSTDBY_Msk;
    while (GCLK_REGS->GCLK_SYNCBUSY & GCLK_SYNCBUSY_GENCTRL0_Msk) {
    }
    OSCCTRL_REGS->OSCCTRL_OSCHFCTRL |= OSCCTRL_OSCHFCTRL_ONDEMAND_Msk;
}

static void stest_cmd(int argc, char **argv)
{
    if (argc != 2) {
        printk("usage: stest <idle|idledeep|sb0|sbq|genoff|off>\n");
        return;
    }

    const struct sleep_cfg *m = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(modes); i++) {
        if (strcmp(argv[1], modes[i].name) == 0) {
            m = &modes[i];
            break;
        }
    }
    if (m == NULL) {
        printk("stest: unknown mode '%s'\n", argv[1]);
        return;
    }

    printk("stest: mode=%s sleepdeep=%d pm=%u gate=%u wake=%s\n",
           m->name, m->sleepdeep, m->pm_mode, m->gate,
           m->wake_wdt ? "wdt-reset" : "NONE(ext-reset-only)");
    if (!m->wake_wdt) {
        printk("stest: core will stay asleep until a debugger reset.\n");
    }
    led_ctrl_off();
    k_msleep(50); /* let the console drain before we gate it */

    /* Only the WDT (a reset) may end this. Stop SysTick so its tick can't wake
     * WFI early; lock interrupts. */
    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
    unsigned int key = irq_lock();

    if (m->wake_wdt) {
        wdt_arm_reset();
    }

    /* sleep-mode selection */
    PM_REGS->PM_SLEEPCFG = m->pm_mode;
    while ((PM_REGS->PM_SLEEPCFG & PM_SLEEPCFG_SLEEPMODE_Msk) != m->pm_mode) {
    }
    if (m->sleepdeep) {
        SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    } else {
        SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
    }

    if (m->gate >= 1U) {
        quiesce_clock_tree();
    }
    if (m->gate >= 2U) {
        /* stop the CPU clock generator itself; the core halts here and the WDT
         * (if armed) resets it. */
        GCLK_REGS->GCLK_GENCTRL[0] &= ~GCLK_GENCTRL_GENEN_Msk;
    }

    for (;;) {
        __DSB();
        __WFI();
    }
    /* never returns; wakes only via WDT reset or external reset */
    (void)key;
}
CMD_REGISTER(stest, "stest", stest_cmd,
         "stest <mode>    - sleep experiment: idle|idledeep|sb0|sbq|genoff|off");
