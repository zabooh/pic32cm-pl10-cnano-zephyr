/*
 * diag.c - system / diagnostic console commands: reset, threads, mem.
 *
 * These are not tied to any feature peripheral, so they live here (rather than
 * in the parser) and self-register with the command registry (cmd.h), keeping
 * cmd_parser.c free of any command-specific knowledge.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <stdlib.h>

#include "cmd.h"

static void reset_cmd(int argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    printk("resetting...\n");
    sys_reboot(SYS_REBOOT_COLD);
}
CMD_REGISTER(reset, "reset", reset_cmd, "reset           - reboot the board");

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

static void threads_cmd(int argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    printk("Threads (used/total stack):\n");
    k_thread_foreach(thread_info_cb, NULL);
}
CMD_REGISTER(threads, "threads", threads_cmd, "threads         - list threads with live stack usage");

/* Raw memory hex-dump. Deliberately does NO address validation - reading an
 * unmapped, peripheral, or misaligned address will bus-fault (HardFault) on
 * this MPU-less Cortex-M0+, which is intentional here: it lets you provoke and
 * study faults (pairs with CONFIG_STACK_SENTINEL / the fatal-error handler).
 * For a normal dump use a valid RAM (0x20000000) or flash (0x0c000000)
 * address. `volatile` so the read actually happens even if the value is
 * unused (a faulting access must not be optimized away). */
static void mem_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: mem <hex-addr> [count]\n");
        return;
    }

    uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t count = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : 64;

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
CMD_REGISTER(mem, "mem", mem_cmd, "mem <addr> [n]  - hex-dump n bytes at hex <addr> (no bounds check, can fault)");
