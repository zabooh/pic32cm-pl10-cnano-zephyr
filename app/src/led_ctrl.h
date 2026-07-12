#ifndef LED_CTRL_H_
#define LED_CTRL_H_

#include <stdint.h>

/* LED0 hardware ownership (GPIO + the dedicated blink thread) lives in
 * led_ctrl.c; this is the only interface the rest of the app uses. */

/* Configure the LED GPIO. Returns 0 on success, negative errno on failure
 * (LED device not ready). Call once at startup before any other function. */
int led_ctrl_init(void);

void led_ctrl_on(void);
void led_ctrl_off(void);
void led_ctrl_toggle(void);

/* Blink every ms milliseconds; 0 stops blinking. */
void led_ctrl_blink(uint32_t ms);

#endif /* LED_CTRL_H_ */
