#include "led_ctrl.h"

/*
 * The application is built from self-contained modules that each start their
 * own thread (led_ctrl.c's blink thread, cmd_parser.c's command thread,
 * pl10_adc.c's stream thread). main() only wires up startup policy: bring the
 * LED up and set its default state.
 */
int main(void)
{
    if (led_ctrl_init() != 0) {
        return 0;
    }
    led_ctrl_blink(500);
    return 0;
}
