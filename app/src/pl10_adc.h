#ifndef PL10_ADC_H_
#define PL10_ADC_H_

#include <stdint.h>

/*
 * Stopgap ADC0 access for the PIC32CM PL10, ported from the bare-metal CMSIS
 * reference in C:\work\Bukarest\3_Mi_CMSIS\. Zephyr has no adc_driver_api/
 * devicetree binding for this SoC's ADC yet (its register map doesn't match
 * either existing in-tree Microchip ADC driver - see conversation history).
 * Replace this with a proper Zephyr driver once upstream support lands.
 */

/* The only AIN channel this board actually breaks out (PA29/pin 39) - see
 * pl10_adc.c for how that was confirmed. */
#define PL10_ADC_AIN29 29U

void pl10_adc_init(void);

/* Blocking single-ended read on one AIN channel (ADC_INPUTCTRL_MUXPOS_AINx). */
uint16_t pl10_adc_read(uint8_t ain_channel);

#endif /* PL10_ADC_H_ */
