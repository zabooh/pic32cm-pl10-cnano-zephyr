#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>

#include "led_ctrl.h"

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define BLINK_THREAD_STACK_SIZE 256
#define BLINK_THREAD_PRIORITY 7

/* Blink interval in ms; 0 = no automatic blinking */
static volatile uint32_t blink_ms;

static void blink_thread_entry(void *p1, void *p2, void *p3)
{
    while (1) {
        uint32_t ms = blink_ms;

        if (ms == 0) {
            k_sleep(K_FOREVER);
            continue;
        }

        /* Only toggle if the sleep ran to completion - if led_ctrl_blink()
         * woke us up early (rate changed, or blinking stopped), re-read
         * blink_ms and start over instead of toggling on stale settings. */
        if (k_sleep(K_MSEC(ms)) == 0) {
            gpio_pin_toggle_dt(&led);
        }
    }
}
K_THREAD_DEFINE(blink_tid, BLINK_THREAD_STACK_SIZE, blink_thread_entry, NULL, NULL, NULL,
        BLINK_THREAD_PRIORITY, 0, 0);

void led_ctrl_blink(uint32_t ms)
{
    blink_ms = ms;
    k_wakeup(blink_tid);
}

void led_ctrl_on(void)
{
    led_ctrl_blink(0);
    gpio_pin_set_dt(&led, 1);
}

void led_ctrl_off(void)
{
    led_ctrl_blink(0);
    gpio_pin_set_dt(&led, 0);
}

void led_ctrl_toggle(void)
{
    led_ctrl_blink(0);
    gpio_pin_toggle_dt(&led);
}

int led_ctrl_init(void)
{
    if (!gpio_is_ready_dt(&led)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    return 0;
}
