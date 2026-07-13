#include <zephyr/kernel.h>
#include <zephyr/console/console.h>
#include <zephyr/version.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "app_threads.h"
#include "cmd.h"

/*
 * Generic console front-end: a hand-rolled line editor (with bash-style Up/Down
 * history) over console_getchar(), plus a tokenizer that dispatches each line
 * to the command registry (cmd.h). This module knows nothing about led/adc/etc.
 * - every command lives in its owning module and self-registers via
 * CMD_REGISTER(); adding a command never touches this file. Only "help" is
 * built in here, since it is intrinsic to the registry.
 */

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
#define CMD_MAX_ARGS 4 /* deepest: "adc stream start" / "mem <addr> <n>" = 3 */

#define KEY_BS 0x08
#define KEY_DEL 0x7f
#define KEY_ESC 0x1b
#define KEY_CSI '['
#define KEY_UP 'A'
#define KEY_DOWN 'B'

/* Split `line` into argv[] in place (spaces overwritten with '\0'). Returns
 * argc; extra tokens beyond `max` are left attached to the last argv entry. */
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;

    while (*line != '\0' && argc < max) {
        while (*line == ' ') {
            *line++ = '\0';
        }
        if (*line == '\0') {
            break;
        }
        argv[argc++] = line;
        while (*line != '\0' && *line != ' ') {
            line++;
        }
    }
    return argc;
}

static void handle_line(char *line)
{
    char *argv[CMD_MAX_ARGS];
    int argc = tokenize(line, argv, CMD_MAX_ARGS);

    if (argc == 0) {
        return; /* empty line */
    }

    STRUCT_SECTION_FOREACH(cmd, c) {
        if (strcmp(argv[0], c->name) == 0) {
            c->fn(argc, argv);
            return;
        }
    }
    printk("unknown command: %s\n", argv[0]);
}

/* "help" is built in here: it iterates the command registry and prints each
 * entry's one-line help. Registration order is link order (not sorted). */
static void help_cmd(int argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    printk("Available commands:\n");
    STRUCT_SECTION_FOREACH(cmd, c) {
        printk("  %s\n", c->help);
    }
    printk("  (Up/Down arrow recalls the last %u commands)\n", CMD_HISTORY_DEPTH);
}
CMD_REGISTER(help, "help", help_cmd, "help            - show this help");

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
