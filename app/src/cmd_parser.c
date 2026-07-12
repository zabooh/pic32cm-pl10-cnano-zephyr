#include <zephyr/kernel.h>
#include <zephyr/console/console.h>
#include <zephyr/sys/reboot.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "led_ctrl.h"
#include "pl10_adc.h"

#define PROMPT "pl10:~$ "

#define ADC_VREF_MV 3300U
#define ADC_STREAM_PERIOD_MS 500U

#define ADC_STREAM_THREAD_STACK_SIZE 320
#define ADC_STREAM_THREAD_PRIORITY 7

#define CMD_THREAD_STACK_SIZE 640
#define CMD_THREAD_PRIORITY 7

#define CMD_LINE_MAX_LEN 32
#define CMD_HISTORY_DEPTH 5

#define KEY_BS 0x08
#define KEY_DEL 0x7f
#define KEY_ESC 0x1b
#define KEY_CSI '['
#define KEY_UP 'A'
#define KEY_DOWN 'B'

static void cmd_led_blink(const char *arg)
{
    if (arg == NULL || *arg == '\0') {
        printk("usage: led blink <ms>\n");
        return;
    }
    uint32_t ms = (uint32_t)strtoul(arg, NULL, 10);
    led_ctrl_blink(ms);
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

/* Enabled by "adc stream start", disabled by "adc stream stop" - see
 * adc_stream_set(). */
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

        /* If adc_stream_set() wakes us early (stopped mid-period), loop
         * back around immediately and re-check adc_streaming instead of
         * sampling again on a shortened period. */
        k_sleep(K_MSEC(ADC_STREAM_PERIOD_MS));
    }
}
K_THREAD_DEFINE(adc_stream_tid, ADC_STREAM_THREAD_STACK_SIZE, adc_stream_thread_entry, NULL, NULL,
        NULL, ADC_STREAM_THREAD_PRIORITY, 0, 0);

static void adc_stream_set(bool enable)
{
    adc_streaming = enable;
    k_wakeup(adc_stream_tid);
}

static void cmd_adc_stream(const char *arg)
{
    if (arg != NULL && strcmp(arg, "start") == 0) {
        adc_stream_set(true);
        printk("adc: streaming every %u ms (adc stream stop to stop)\n", ADC_STREAM_PERIOD_MS);
    } else if (arg != NULL && strcmp(arg, "stop") == 0) {
        adc_stream_set(false);
        printk("adc: stream stopped\n");
    } else {
        printk("usage: adc stream <start|stop>\n");
    }
}

static void cmd_reset(void)
{
    printk("resetting...\n");
    sys_reboot(SYS_REBOOT_COLD);
}

/* Ring buffer of the last CMD_HISTORY_DEPTH submitted lines, oldest first. */
static char cmd_history[CMD_HISTORY_DEPTH][CMD_LINE_MAX_LEN];
static uint8_t cmd_history_count;
static uint8_t cmd_history_next;

/* Map a chronological position (0 = oldest kept, cmd_history_count - 1 =
 * newest) to its physical ring-buffer slot. */
static uint8_t cmd_history_index(uint8_t pos)
{
    uint8_t start = (cmd_history_count == CMD_HISTORY_DEPTH) ? cmd_history_next : 0;
    return (start + pos) % CMD_HISTORY_DEPTH;
}

static void cmd_history_add(const char *line)
{
    if (*line == '\0') {
        return;
    }

    strncpy(cmd_history[cmd_history_next], line, CMD_LINE_MAX_LEN - 1);
    cmd_history[cmd_history_next][CMD_LINE_MAX_LEN - 1] = '\0';

    cmd_history_next = (cmd_history_next + 1) % CMD_HISTORY_DEPTH;
    if (cmd_history_count < CMD_HISTORY_DEPTH) {
        cmd_history_count++;
    }
}

static void cmd_history_print(void)
{
    if (cmd_history_count == 0) {
        printk("history: empty\n");
        return;
    }

    for (uint8_t i = 0; i < cmd_history_count; i++) {
        printk("  %u  %s\n", i + 1, cmd_history[cmd_history_index(i)]);
    }
}

static void cmd_help(void)
{
    printk("Available commands:\n"
           "  led on          turn LED on\n"
           "  led off         turn LED off\n"
           "  led toggle      toggle LED\n"
           "  led blink <ms>  blink LED every <ms> ms (0 stops blinking)\n"
           "  adc read        read the ADC (AIN29) once\n"
           "  adc stream start  read the ADC every %u ms\n"
           "  adc stream stop   stop the ADC stream\n"
           "  reset           reboot the board\n"
           "  history         show the last %u commands (also: Up/Down arrow)\n"
           "  help            show this help\n", ADC_STREAM_PERIOD_MS, CMD_HISTORY_DEPTH);
}

static void handle_line(char *line)
{
    if (strcmp(line, "led on") == 0) {
        led_ctrl_on();
        printk("led on\n");
    } else if (strcmp(line, "led off") == 0) {
        led_ctrl_off();
        printk("led off\n");
    } else if (strcmp(line, "led toggle") == 0) {
        led_ctrl_toggle();
        printk("led toggled\n");
    } else if (strncmp(line, "led blink", 9) == 0) {
        char *arg = line + 9;
        while (*arg == ' ') {
            arg++;
        }
        cmd_led_blink(arg);
    } else if (strcmp(line, "adc read") == 0) {
        cmd_adc_read();
    } else if (strncmp(line, "adc stream", 10) == 0) {
        char *arg = line + 10;
        while (*arg == ' ') {
            arg++;
        }
        cmd_adc_stream(arg);
    } else if (strcmp(line, "reset") == 0) {
        cmd_reset();
    } else if (strcmp(line, "history") == 0) {
        cmd_history_print();
    } else if (strcmp(line, "help") == 0) {
        cmd_help();
    } else if (*line != '\0') {
        printk("unknown command: %s\n", line);
    }
}

/*
 * printk() and console_putchar() are two independent output paths on top of
 * the same UART (printk goes straight through uart_console.c's synchronous
 * uart_poll_out(); console_putchar() goes through console_getchar()'s own
 * buffered, interrupt-driven tty layer). Mixing them reorders/drops bytes on
 * the wire, so every echo/redraw byte here goes through printk("%c", ...)
 * instead of console_putchar() - one single output path throughout.
 */
static void echo_char(char c)
{
    printk("%c", c);
}

static void echo_backspace(void)
{
    printk("\b \b");
}

/* Erase `len` already-echoed characters left of the cursor and reprint
 * `text` (`len_new` chars) in their place - works on any plain terminal,
 * no ANSI cursor-positioning required. */
static void redraw_line(size_t len, const char *text, size_t len_new)
{
    for (size_t i = 0; i < len; i++) {
        echo_backspace();
    }
    for (size_t i = 0; i < len_new; i++) {
        echo_char(text[i]);
    }
}

/* Hand-rolled line editor: console_getline() has no Up/Down history recall
 * (Zephyr's console driver silently drops those escape sequences - see
 * conversation history), so this reads character-by-character via
 * console_getchar() instead and implements just enough editing for this
 * app: end-of-line backspace, and bash-style history browsing. No mid-line
 * cursor movement (Left/Right arrows) - out of scope for this minimal
 * parser, same reasoning that kept the Zephyr shell out of this project.
 */
static size_t cmd_read_line(char *buf, size_t buf_size)
{
    /* Persists across calls: a "\r\n" pair spans two console_getchar() calls
     * that can land in two different cmd_read_line() invocations (the '\r'
     * ends this line; the '\n' arrives right at the start of the next
     * read). Remembering the last byte lets that trailing '\n' be swallowed
     * instead of being misread as an Enter on an empty line - same trick
     * Zephyr's own uart_console_isr() uses for CONFIG_CONSOLE_GETLINE. */
    static uint8_t last_char;

    size_t len = 0;
    uint8_t history_pos = cmd_history_count; /* one past newest = "not browsing" */

    buf[0] = '\0';

    while (1) {
        int c = console_getchar();

        if (c < 0) {
            continue;
        }

        if (c == '\n' && last_char == '\r') {
            last_char = (uint8_t)c;
            continue;
        }
        last_char = (uint8_t)c;

        if (c == '\r' || c == '\n') {
            printk("\n");
            buf[len] = '\0';
            return len;
        }

        if (c == KEY_BS || c == KEY_DEL) {
            if (len > 0) {
                len--;
                echo_backspace();
            }
            continue;
        }

        if (c == KEY_ESC) {
            if (console_getchar() != KEY_CSI) {
                continue;
            }
            int key = console_getchar();

            if (key != KEY_UP && key != KEY_DOWN) {
                continue; /* unsupported escape sequence - ignore */
            }

            uint8_t new_pos = history_pos;
            if (key == KEY_UP && history_pos > 0) {
                new_pos = history_pos - 1;
            } else if (key == KEY_DOWN && history_pos < cmd_history_count) {
                new_pos = history_pos + 1;
            }
            if (new_pos == history_pos) {
                continue;
            }
            history_pos = new_pos;

            size_t old_len = len;
            if (history_pos == cmd_history_count) {
                len = 0;
            } else {
                len = strlen(cmd_history[cmd_history_index(history_pos)]);
                memcpy(buf, cmd_history[cmd_history_index(history_pos)], len);
            }
            buf[len] = '\0';
            redraw_line(old_len, buf, len);
            continue;
        }

        if (isprint(c) != 0 && len + 1 < buf_size) {
            buf[len++] = (char)c;
            echo_char((char)c);
        }
    }
}

static void cmd_thread_entry(void *p1, void *p2, void *p3)
{
    console_init();

    static char line[CMD_LINE_MAX_LEN];

    printk("\nPIC32CM PL10 Blinky - built " __DATE__ " " __TIME__ "\n");
    printk(PROMPT);
    while (1) {
        cmd_read_line(line, sizeof(line));
        cmd_history_add(line);
        handle_line(line);
        printk(PROMPT);
    }
}
K_THREAD_DEFINE(cmd_tid, CMD_THREAD_STACK_SIZE, cmd_thread_entry, NULL, NULL, NULL,
        CMD_THREAD_PRIORITY, 0, 0);
