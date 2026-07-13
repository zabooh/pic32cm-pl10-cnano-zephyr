/*
 * fault.c - fatal-error handler that names the offending thread, then reboots.
 *
 * With CONFIG_LOG=n the kernel's own ">>> ZEPHYR FATAL ERROR N" / "Current
 * thread: ..." lines (printed by z_fatal_error() via LOG_ERR) are compiled
 * out, so the only thing on the console is the ARM register ESF dump - which
 * does NOT say which thread faulted. In particular a CONFIG_STACK_SENTINEL
 * stack-overflow (K_ERR_STACK_CHK_FAIL) is then indistinguishable, from the
 * raw dump alone, from a random HardFault, and can't be attributed to a thread.
 *
 * We override the __weak k_sys_fatal_error_handler() (kernel/fatal.c) to print
 * that missing context ourselves via printk() - which is synchronous and works
 * with logging disabled - so an overflow is immediately attributable to a
 * thread by name. It runs AFTER the arch register dump, right before the reset.
 *
 * Reboot behaviour mirrors the default handler and stays governed by
 * CONFIG_RESET_ON_FATAL_ERROR (reset vs. halt), so removing this file falls
 * back to the identical reboot, just without the thread line.
 */
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

static const char *reason_str(unsigned int reason)
{
	switch (reason) {
	case K_ERR_CPU_EXCEPTION:  return "CPU exception";
	case K_ERR_SPURIOUS_IRQ:   return "unhandled interrupt";
	case K_ERR_STACK_CHK_FAIL: return "stack overflow (sentinel corruption)";
	case K_ERR_KERNEL_OOPS:    return "kernel oops";
	case K_ERR_KERNEL_PANIC:   return "kernel panic";
	default:                   return "unknown error";
	}
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	struct k_thread *cur = k_current_get();
	const char *name = k_thread_name_get(cur);

	printk("\n*** FATAL ERROR %u: %s ***\n", reason, reason_str(reason));
	printk("*** in thread: %s (%p) ***\n",
	       (name != NULL && name[0] != '\0') ? name : "unknown", cur);

#if IS_ENABLED(CONFIG_RESET_ON_FATAL_ERROR)
	sys_reboot(SYS_REBOOT_WARM);
#else
	arch_system_halt(reason);
#endif
	CODE_UNREACHABLE;
}
