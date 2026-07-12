#include "pl10_adc.h"

#include <zephyr/kernel.h>

#include <pic32cm6408pl10048.h>

/* ADC0_REGS is provided by pic32cm6408pl10048.h itself. */

/* The only AIN channel this board actually breaks out (PA29/pin 39) - see
 * pl10_adc_pinctrl_enable() below for how that was confirmed. */
#define PL10_ADC_AIN29 29U

#define ADC_VREF_MV 3300U

#define ADC_STREAM_THREAD_STACK_SIZE 320
#define ADC_STREAM_THREAD_PRIORITY 7

/*
 * Zephyr's clock_control driver for this SoC (mchp_clock_pic32cm_pl.h) has no
 * ADC0 subsystem ID - there's no Zephyr ADC devicetree node to generate one
 * from. Adding one would mean patching a pinned, third-party Zephyr header
 * (see CLAUDE.md: only app/ is hand-maintained here), so unlike a peripheral
 * Zephyr's driver already knows about, direct register access is the only
 * option without doing that.
 *
 * Verified against three independent sources, all in agreement:
 *   - Datasheet DS40002667A, 11.6.8 "APB Bridge C Mask" (bit 7 = ADC0) and
 *     33.4.2.5 "ADC Clock" (CLK_ADC is derived by prescaling CLK_ADC0_APB -
 *     no GCLK peripheral channel needed at all, unlike e.g. SERCOM).
 *   - C:\work\Bukarest\3_Mi_CMSIS\CNANO_MCHP_Driver_Example\main.c:
 *     MCLK_EnableAPBCClock(MCLK_APBC_ADC0), no GCLK call for the ADC.
 *   - The official Microchip Harmony reference app for this exact board
 *     (github.com/Microchip-MPLAB-Harmony/csp_apps_pic32cm_pl10 v1.0.0,
 *     apps/adc/adc_sample/pic32cm_pl10_curiosity_nano.X), whose generated
 *     plib_clock.c does `MCLK_REGS->MCLK_APBCMASK = 0x884U;` - bit 7 (0x80)
 *     set, and only one GCLK_PCHCTRL call in the whole file, for SERCOM1.
 * |= instead of a plain assignment so this doesn't clobber whatever bits
 * Zephyr's own clock_control driver already set for other peripherals in the
 * same register.
 */
static void pl10_adc_clock_enable(void)
{
    MCLK_REGS->MCLK_APBCMASK |= MCLK_APBCMASK_ADC0_Msk;
}

/*
 * PA29 (pin 39) / AIN29, peripheral function B (analog) - confirmed by the
 * same Harmony reference app's pin_configurations.csv ("39,PA29,,ADC0_AIN29,
 * Analog,High Impedance") and its generated plib_port.c
 * (PORT_PINCFG[29] |= PMUXEN, PORT_PMUX[14] upper nibble = 0x1 = function B).
 * The Bukarest CMSIS reference uses the same pin - it turns out AIN29/PA29
 * is simply the one ADC-capable pin this board itself breaks out (the
 * potentiometer the Bukarest comment mentions lives on the separate
 * Curiosity Nano Explorer carrier board, wired to this same edge pin).
 *
 * PORT_PMUX[14] covers the PA28/PA29 pin pair as two nibbles; only the
 * upper (PA29) nibble is touched here so an unrelated future use of PA28
 * isn't clobbered.
 */
static void pl10_adc_pinctrl_enable(void)
{
    PORT_REGS->GROUP[0].PORT_PINCFG[29] |= PORT_PINCFG_PMUXEN_Msk;
    PORT_REGS->GROUP[0].PORT_PMUX[14] = (PORT_REGS->GROUP[0].PORT_PMUX[14] & 0x0FU) | 0x10U;
}

/* --- low-level register driver --------------------------------------- */

static void pl10_adc_init(void)
{
    pl10_adc_clock_enable();
    pl10_adc_pinctrl_enable();

    ADC0_REGS->ADC_CTRLA = ADC_CTRLA_SWRST_Msk;
    while (ADC0_REGS->ADC_CTRLA & ADC_CTRLA_SWRST_Msk) {
    }

    ADC0_REGS->ADC_CTRLB = ADC_CTRLB_PRESCALER_DIV8;
    ADC0_REGS->ADC_CTRLC = ADC_CTRLC_REFSEL_VDD;
    ADC0_REGS->ADC_CTRLD = ADC_CTRLD_RESOLUTION_12BIT | ADC_CTRLD_SAMPNUM_NONE;
    ADC0_REGS->ADC_CTRLE = ADC_CTRLE_SAMPLEN(4);

    ADC0_REGS->ADC_CTRLA |= ADC_CTRLA_ENABLE_Msk;
}

/* Blocking single-ended read on one AIN channel (ADC_INPUTCTRL_MUXPOS_AINx). */
static uint16_t pl10_adc_read(uint8_t ain_channel)
{
    ADC0_REGS->ADC_INPUTCTRL = ADC_INPUTCTRL_MUXPOS(ain_channel) | ADC_INPUTCTRL_MUXNEG_GND;

    ADC0_REGS->ADC_INTFLAG = ADC_INTFLAG_RESRDY_Msk;
    ADC0_REGS->ADC_COMMAND = ADC_COMMAND_MODE_SINGLE | ADC_COMMAND_START_IMMEDIATE;

    while (!(ADC0_REGS->ADC_INTFLAG & ADC_INTFLAG_RESRDY_Msk)) {
    }

    return (uint16_t)(ADC0_REGS->ADC_RESULT & 0xFFFF);
}

/* --- application-level ADC service ------------------------------------ */

static bool adc_ready;

static void ensure_adc_ready(void)
{
    if (!adc_ready) {
        pl10_adc_init();
        adc_ready = true;
    }
}

static void print_adc_sample(uint16_t counts)
{
    uint32_t mv = ((uint32_t)counts * ADC_VREF_MV) / 4095U;
    printk("adc: %u (%u.%03u V)\n", counts, mv / 1000U, mv % 1000U);
}

void pl10_adc_read_once(void)
{
    ensure_adc_ready();
    print_adc_sample(pl10_adc_read(PL10_ADC_AIN29));
}

/* Enabled/disabled by pl10_adc_stream_set(). */
static volatile bool adc_streaming;

static void adc_stream_thread_entry(void *p1, void *p2, void *p3)
{
    while (1) {
        if (!adc_streaming) {
            k_sleep(K_FOREVER);
            continue;
        }

        ensure_adc_ready();
        print_adc_sample(pl10_adc_read(PL10_ADC_AIN29));

        /* If pl10_adc_stream_set() wakes us early (stopped mid-period), loop
         * back around immediately and re-check adc_streaming instead of
         * sampling again on a shortened period. */
        k_sleep(K_MSEC(PL10_ADC_STREAM_PERIOD_MS));
    }
}
K_THREAD_DEFINE(adc_stream_tid, ADC_STREAM_THREAD_STACK_SIZE, adc_stream_thread_entry, NULL, NULL,
        NULL, ADC_STREAM_THREAD_PRIORITY, 0, 0);

void pl10_adc_stream_set(bool enable)
{
    adc_streaming = enable;
    k_wakeup(adc_stream_tid);

    if (enable) {
        printk("adc: streaming every %u ms\n", PL10_ADC_STREAM_PERIOD_MS);
    } else {
        printk("adc: streaming stopped\n");
    }
}
