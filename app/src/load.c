/*
 * load.c - a CPU-load generator exposed as the `load` console command, for
 * measuring the difference between the idle (WFI) and active current draw of
 * the PL10.
 *
 * Normally this firmware is almost always asleep: every thread does a little
 * work and then k_sleep()s, so the idle thread runs and executes WFI, halting
 * the core (see DEEPDIVE.md "Idle, WFI, and CPU load"). The datasheet puts that
 * Idle current at ~1.2 mA vs. ~5.2 mA for a 100%-busy core, so keeping the CPU
 * busy should raise the board's current draw by ~4 mA - visible in mA
 * resolution even on the USB port (where the nEDBG debugger + LED dominate the
 * absolute figure, but the *delta* from load on/off still shows).
 *
 * `load on` starts a busy loop that prevents the core from ever reaching WFI;
 * `load off` stops it. The generator runs in its own thread (load_tid) at a
 * priority *below* the application threads (LOAD_THREAD_PRIORITY, see
 * app_threads.h): it therefore only consumes time that would otherwise be idle,
 * so the console, blink, ADC and button threads (all priority 7) still preempt
 * it instantly and stay fully responsive. When load is off the thread blocks on
 * a semaphore and consumes nothing, so the idle thread's WFI returns.
 *
 * Self-registered via CMD_REGISTER, like the other feature modules.
 */
#include <zephyr/kernel.h>
#include <string.h>

#include "app_threads.h"
#include "cmd.h"

/* Set when load is active. volatile so the busy loop re-reads it every
 * iteration (so `load off` takes effect at once, and the loop can't be
 * optimized away). */
static volatile bool load_on;

/* Sink for the busy-loop result. volatile so the compiler can't discard the
 * computation as dead code. */
static volatile uint32_t load_sink;

/* Released by `load on` to wake the generator thread out of its idle block. */
static K_SEM_DEFINE(load_sem, 0, 1);

static void load_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printk("load: %s\n", load_on ? "on" : "off");
        printk("usage: load on|off\n");
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        if (!load_on) {
            load_on = true;
            k_sem_give(&load_sem); /* unblock the generator thread */
        }
        printk("load: on - CPU held busy (no WFI); expect ~4 mA higher draw\n");
        printk("load: tip - 'led off' to stop the blink for a cleaner baseline\n");
    } else if (strcmp(argv[1], "off") == 0) {
        load_on = false; /* generator returns to its semaphore block */
        printk("load: off - core back to WFI idle\n");
    } else {
        printk("usage: load on|off\n");
    }
}
CMD_REGISTER(load, "load", load_cmd, "load on|off     - burn CPU cycles to raise current draw");

/* --- CPU-load generator thread ------------------------------------------ */

static void load_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    uint32_t acc = 1;

    while (1) {
        /* Idle: block until `load on` gives the semaphore. Consumes no CPU,
         * so the kernel idle thread runs and the core reaches WFI. */
        k_sem_take(&load_sem, K_FOREVER);

        /* Busy: a self-feeding LCG recurrence (can't be constant-folded) that
         * runs flat out until `load off` clears the flag. Being lower priority
         * than the app threads, this only replaces idle time - the console
         * still preempts it to handle `load off`. */
        while (load_on) {
            acc = acc * 1664525u + 1013904223u;
            load_sink = acc;
        }
    }
}
K_THREAD_DEFINE(load_tid, LOAD_THREAD_STACK_SIZE, load_thread_entry, NULL, NULL, NULL,
        LOAD_THREAD_PRIORITY, 0, 0);
