#ifndef PL10_ADC_H_
#define PL10_ADC_H_

#include <stdbool.h>

/*
 * Stopgap ADC0 support for the PIC32CM PL10, ported from the bare-metal CMSIS
 * reference in C:\work\Bukarest\3_Mi_CMSIS\. Zephyr has no adc_driver_api/
 * devicetree binding for this SoC's ADC yet (its register map doesn't match
 * either existing in-tree Microchip ADC driver - see conversation history).
 * Replace this with a proper Zephyr driver once upstream support lands.
 *
 * This module owns everything ADC: the low-level register driver plus the
 * application-level read/stream service below. The command parser only ever
 * talks to this interface - it holds no ADC state or hardware knowledge of
 * its own.
 */

/* Period of the background sampling stream, in milliseconds. */
#define PL10_ADC_STREAM_PERIOD_MS 500U

/* One-shot: read the ADC once and print the formatted result. Lazily
 * initializes the hardware on first use. */
void pl10_adc_read_once(void);

/* Start (enable = true) or stop the background sampling stream, which prints
 * one formatted result every PL10_ADC_STREAM_PERIOD_MS. */
void pl10_adc_stream_set(bool enable);

#endif /* PL10_ADC_H_ */
