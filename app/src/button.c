/*
 * button.c - read the on-board user switch SW0 and expose it as the `button`
 * console command.
 *
 * SW0 is on PB03 (PIC32CM PL10 Curiosity Nano User Guide DS50004003, section 4.2
 * "Mechanical Switch"). Pressing it pulls PB03 to GND, and the board has no
 * external pull-up, so the pin is configured active-low with the MCU's internal
 * pull-up enabled - both flags come from the devicetree node in app/app.overlay
 * (Zephyr's board support defines only led0, not the switch). Because
 * gpio_pin_get_dt() returns the *logical* level, GPIO_ACTIVE_LOW is handled for
 * us: 1 means pressed.
 *
 * Two ways to observe it: the `button` console command reads the current state
 * on demand, and a small background thread (button_tid) watches for edges and
 * prints a message when the button is pressed or released - asynchronously,
 * like pl10_adc.c's stream. GPIO pin interrupts aren't an option here: the
 * PL10's pin-interrupt path needs the EIC (CONFIG_INTC_MCHP_EIC_G1), and this
 * Zephyr revision has no EIC devicetree node for the PL10, so
 * gpio_pin_interrupt_configure() returns -ENOTSUP. Polling every 50 ms is well
 * within a human press and also debounces the switch (contact bounce is far
 * shorter than the poll interval).
 *
 * Self-registered via CMD_REGISTER (like diag.c); the pin is configured lazily
 * on first use (mirrors pl10_adc.c's ensure_adc_ready) so main.c stays
 * untouched.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>

#include "app_threads.h"
#include "cmd.h"

#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec sw0 = GPIO_DT_SPEC_GET(SW0_NODE, gpios);

static bool sw0_ready;

static int ensure_sw0_ready(void)
{
    if (sw0_ready) {
        return 0;
    }
    if (!gpio_is_ready_dt(&sw0)) {
        return -ENODEV;
    }

    /* GPIO_INPUT combines with the node's dt_flags (GPIO_ACTIVE_LOW |
     * GPIO_PULL_UP), so the internal pull-up is enabled here. */
    int ret = gpio_pin_configure_dt(&sw0, GPIO_INPUT);
    if (ret != 0) {
        return ret;
    }

    sw0_ready = true;
    return 0;
}

static void button_cmd(int argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (ensure_sw0_ready() != 0) {
        printk("button: not available\n");
        return;
    }

    int val = gpio_pin_get_dt(&sw0);
    if (val < 0) {
        printk("button: read error %d\n", val);
        return;
    }

    printk("button: %s\n", val ? "pressed" : "released");
}
CMD_REGISTER(button, "button", button_cmd, "button          - read the SW0 user button");

/* --- background press/release notifier ---------------------------------- */

#define BUTTON_POLL_MS 50

static void button_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (ensure_sw0_ready() != 0) {
        return; /* pin unavailable - nothing to watch */
    }

    /* Seed with the current level so we only report actual changes. */
    int prev = gpio_pin_get_dt(&sw0);

    while (1) {
        k_sleep(K_MSEC(BUTTON_POLL_MS));

        int cur = gpio_pin_get_dt(&sw0);
        if (cur < 0 || cur == prev) {
            continue;
        }

        printk("button: %s\n", cur ? "pressed" : "released");
        prev = cur;
    }
}
K_THREAD_DEFINE(button_tid, BUTTON_THREAD_STACK_SIZE, button_thread_entry, NULL, NULL, NULL,
        BUTTON_THREAD_PRIORITY, 0, 0);
