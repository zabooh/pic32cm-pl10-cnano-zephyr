#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/console/console.h>
#include <zephyr/sys/reboot.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pl10_adc.h"

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define PROMPT "pl10:~$ "

#define ADC_VREF_MV 3300U
#define ADC_STREAM_PERIOD_MS 500U
#define ADC_STREAM_POLL_MS 10U

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

static void cmd_adc_read(void)
{
    ensure_adc_ready();
    print_adc_sample(pl10_adc_read(PL10_ADC_AIN29));
}

static void cmd_adc_stream(void)
{
    ensure_adc_ready();

    const struct device *console_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

    printk("adc: streaming every %u ms, press Ctrl+C to stop\n", ADC_STREAM_PERIOD_MS);

    /*
     * console_getline()'s input ISR (uart_console_isr(), see
     * zephyr/drivers/console/uart_console.c) silently drops Ctrl+C (0x03) -
     * it falls through to "default: break" there, so this command can't see
     * it through console_getline(). Steal the UART RX interrupt for the
     * duration of the stream and poll for the byte ourselves instead; hand
     * it back to the getline ISR when done.
     */
    uart_irq_rx_disable(console_dev);

    bool stop = false;
    while (!stop) {
        print_adc_sample(pl10_adc_read(PL10_ADC_AIN29));

        for (uint32_t waited = 0; waited < ADC_STREAM_PERIOD_MS; waited += ADC_STREAM_POLL_MS) {
            unsigned char byte;
            if (uart_poll_in(console_dev, &byte) == 0 && byte == 0x03) {
                stop = true;
                break;
            }
            k_msleep(ADC_STREAM_POLL_MS);
        }
    }

    /* Drain any leftover bytes (e.g. an Enter typed right after Ctrl+C) so
     * console_getline() doesn't see a stray line once it resumes. */
    unsigned char discard;
    while (uart_poll_in(console_dev, &discard) == 0) {
    }

    uart_irq_rx_enable(console_dev);
    printk("adc: stream stopped\n");
}

static void cmd_reset(void)
{
    printk("resetting...\n");
    sys_reboot(SYS_REBOOT_COLD);
}

static void cmd_help(void)
{
    printk("Available commands:\n"
           "  led on          turn LED on\n"
           "  led off         turn LED off\n"
           "  led toggle      toggle LED\n"
           "  led blink <ms>  blink LED every <ms> ms (0 stops blinking)\n"
           "  adc read        read the ADC (AIN29) once\n"
           "  adc stream      read the ADC every %u ms until Ctrl+C\n"
           "  reset           reboot the board\n"
           "  help            show this help\n", ADC_STREAM_PERIOD_MS);
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
    } else if (strcmp(line, "adc read") == 0) {
        cmd_adc_read();
    } else if (strcmp(line, "adc stream") == 0) {
        cmd_adc_stream();
    } else if (strcmp(line, "reset") == 0) {
        cmd_reset();
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

    printk("\nPIC32CM PL10 Blinky - built " __DATE__ " " __TIME__ "\n");
    printk(PROMPT);
    while (1) {
        char *line = console_getline();
        handle_line(line);
        printk(PROMPT);
    }
    return 0;
}
