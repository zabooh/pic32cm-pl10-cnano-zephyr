#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/console/console.h>
#include <stdlib.h>
#include <string.h>

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define PROMPT "pl10:~$ "

/* Blink interval in ms; 0 = no automatic blinking */
static volatile uint32_t blink_ms;

static void blink_timer_handler(struct k_timer *timer)
{
    gpio_pin_toggle_dt(&led);
}
K_TIMER_DEFINE(blink_timer, blink_timer_handler, NULL);

static void led_set_blink(uint32_t ms)
{
    blink_ms = ms;
    if (ms > 0) {
        k_timer_start(&blink_timer, K_MSEC(ms), K_MSEC(ms));
    } else {
        k_timer_stop(&blink_timer);
    }
}

static void cmd_led_on(void)
{
    led_set_blink(0);
    gpio_pin_set_dt(&led, 1);
    printk("led on\n");
}

static void cmd_led_off(void)
{
    led_set_blink(0);
    gpio_pin_set_dt(&led, 0);
    printk("led off\n");
}

static void cmd_led_toggle(void)
{
    led_set_blink(0);
    gpio_pin_toggle_dt(&led);
    printk("led toggled\n");
}

static void cmd_led_blink(const char *arg)
{
    if (arg == NULL || *arg == '\0') {
        printk("usage: led blink <ms>\n");
        return;
    }
    uint32_t ms = (uint32_t)strtoul(arg, NULL, 10);
    led_set_blink(ms);
    printk("led blink %u ms\n", ms);
}

static void cmd_help(void)
{
    printk("Available commands:\n"
           "  led on          turn LED on\n"
           "  led off         turn LED off\n"
           "  led toggle      toggle LED\n"
           "  led blink <ms>  blink LED every <ms> ms (0 stops blinking)\n"
           "  help            show this help\n");
}

static void handle_line(char *line)
{
    if (strcmp(line, "led on") == 0) {
        cmd_led_on();
    } else if (strcmp(line, "led off") == 0) {
        cmd_led_off();
    } else if (strcmp(line, "led toggle") == 0) {
        cmd_led_toggle();
    } else if (strncmp(line, "led blink", 9) == 0) {
        char *arg = line + 9;
        while (*arg == ' ') {
            arg++;
        }
        cmd_led_blink(arg);
    } else if (strcmp(line, "help") == 0) {
        cmd_help();
    } else if (*line != '\0') {
        printk("unknown command: %s\n", line);
    }
}

int main(void)
{
    if (!gpio_is_ready_dt(&led)) {
        return 0;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    led_set_blink(500);

    console_getline_init();

    printk("\n" PROMPT);
    while (1) {
        char *line = console_getline();
        handle_line(line);
        printk(PROMPT);
    }
    return 0;
}
