#include <zephyr/kernel.h>
#include <zephyr/console/console.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/version.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_threads.h"
#include "led_ctrl.h"
#include "pl10_adc.h"

/*
 * We print the boot banner ourselves (CONFIG_BOOT_BANNER=n) so it can start
 * with a leading newline - Zephyr's own banner has no leading '\n', so after
 * a fault-triggered reboot it glued onto the tail of the (non-newline-
 * terminated) fault dump. Version string derived exactly as Zephyr's
 * boot_banner.c does. */
#if defined(BUILD_VERSION) && !IS_EMPTY(BUILD_VERSION)
#define ZEPHYR_VERSION_STR STRINGIFY(BUILD_VERSION)
#else
#define ZEPHYR_VERSION_STR KERNEL_VERSION_STRING
#endif

#define PROMPT "pl10:~$ "

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

static void cmd_adc_stream(const char *arg)
{
    if (arg != NULL && strcmp(arg, "start") == 0) {
        pl10_adc_stream_set(true);
    } else if (arg != NULL && strcmp(arg, "stop") == 0) {
        pl10_adc_stream_set(false);
    } else {
        printk("usage: adc stream <start|stop>\n");
    }
}

static void cmd_reset(void)
{
    printk("resetting...\n");
    sys_reboot(SYS_REBOOT_COLD);
}

/* k_thread_foreach() callback: print one thread's name, priority, and live
 * stack usage. Needs CONFIG_THREAD_MONITOR (thread list), THREAD_NAME
 * (names), THREAD_STACK_INFO + INIT_STACKS (accurate used-stack count) -
 * see prj.conf. */
static void thread_info_cb(const struct k_thread *thread, void *user_data)
{
    ARG_UNUSED(user_data);

    size_t unused = 0;
    (void)k_thread_stack_space_get(thread, &unused);

    size_t total = thread->stack_info.size;
    const char *name = k_thread_name_get((k_tid_t)thread);

    printk("  %-14s prio %2d  stack %u/%u B\n",
           (name != NULL && name[0] != '\0') ? name : "?",
           k_thread_priority_get((k_tid_t)thread),
           (unsigned int)(total - unused), (unsigned int)total);
}

static void cmd_threads(void)
{
    printk("Threads (used/total stack):\n");
    k_thread_foreach(thread_info_cb, NULL);
}

/* Raw memory hex-dump. Deliberately does NO address validation - reading an
 * unmapped, peripheral, or misaligned address will bus-fault (HardFault) on
 * this MPU-less Cortex-M0+, which is intentional here: it lets you provoke and
 * study faults (pairs with CONFIG_STACK_SENTINEL / the fatal-error handler).
 * For a normal dump use a valid RAM (0x20000000) or flash (0x0c000000)
 * address. `volatile` so the read actually happens even if the value is
 * unused (a faulting access must not be optimized away). */
static void cmd_mem(const char *arg)
{
    if (arg == NULL || *arg == '\0') {
        printk("usage: mem <hex-addr> [count]\n");
        return;
    }

    char *endp;
    uint32_t addr = (uint32_t)strtoul(arg, &endp, 16);

    uint32_t count = 64;
    while (*endp == ' ') {
        endp++;
    }
    if (*endp != '\0') {
        count = (uint32_t)strtoul(endp, NULL, 0);
    }
    if (count == 0) {
        count = 64;
    }

    const volatile uint8_t *p = (const volatile uint8_t *)addr;

    for (uint32_t i = 0; i < count; i += 16) {
        printk("%08x: ", addr + i);

        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < count) {
                printk("%02x ", p[i + j]);
            } else {
                printk("   ");
            }
        }

        printk(" |");
        for (uint32_t j = 0; j < 16 && i + j < count; j++) {
            uint8_t c = p[i + j];
            printk("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printk("|\n");
    }
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
           "  threads         list threads with live stack usage\n"
           "  mem <addr> [n]  hex-dump n bytes at hex <addr> (no bounds check - can fault)\n"
           "  help            show this help (Up/Down arrow recalls the last %u commands)\n",
           PL10_ADC_STREAM_PERIOD_MS, CMD_HISTORY_DEPTH);
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
        pl10_adc_read_once();
    } else if (strncmp(line, "adc stream", 10) == 0) {
        char *arg = line + 10;
        while (*arg == ' ') {
            arg++;
        }
        cmd_adc_stream(arg);
    } else if (strcmp(line, "reset") == 0) {
        cmd_reset();
    } else if (strcmp(line, "threads") == 0) {
        cmd_threads();
    } else if (strncmp(line, "mem", 3) == 0 && (line[3] == '\0' || line[3] == ' ')) {
        char *arg = line + 3;
        while (*arg == ' ') {
            arg++;
        }
        cmd_mem(arg);
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

    /* Leading '\n' (printk emits it as CR+LF) so the banner always starts on
     * a fresh line, even right after a non-newline-terminated fault dump. */
    printk("\n*** Booting Zephyr OS build " ZEPHYR_VERSION_STR " ***\n"
           "PIC32CM PL10 Blinky - built " __DATE__ " " __TIME__ "\n");
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
