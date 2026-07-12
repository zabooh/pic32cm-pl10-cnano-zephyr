#ifndef LED_CTRL_H_
#define LED_CTRL_H_

#include <stdint.h>

/* LED0 hardware ownership (GPIO + the dedicated blink thread) lives in
 * main.c; this is the only interface the command parser needs against it. */

void led_ctrl_on(void);
void led_ctrl_off(void);
void led_ctrl_toggle(void);

/* Blink every ms milliseconds; 0 stops blinking. */
void led_ctrl_blink(uint32_t ms);

#endif /* LED_CTRL_H_ */
