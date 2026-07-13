#ifndef CMD_H_
#define CMD_H_

#include <zephyr/sys/iterable_sections.h>

/*
 * Console command registry. Each feature module registers its own top-level
 * command with CMD_REGISTER(); the parser (cmd_parser.c) only tokenizes the
 * line, looks up argv[0] in this registry and dispatches - it has no knowledge
 * of led/adc/etc. The registration table lives in a custom iterable ROM
 * section ("cmd"), built at link time, so it costs ~0 RAM (same mechanism as
 * Zephyr's SHELL_CMD_REGISTER, without the shell subsystem's overhead). The
 * linker section is declared in app/cmd_sections.ld (wired in via
 * zephyr_linker_sources() in app/CMakeLists.txt).
 */

/* Handler for one command. argv[0] is the command name; argv[1..argc-1] are
 * its arguments. Handlers print their own output/usage (hence void). */
typedef void (*cmd_fn_t)(int argc, char **argv);

struct cmd {
	const char *name; /* first token to match, e.g. "led" */
	cmd_fn_t fn;      /* dispatched handler */
	const char *help; /* one-line help, printed by the "help" command */
};

/*
 * Register a command. `id` must be unique within its source file (it forms the
 * variable name). `const` is mandatory: it places the entry in the ROM "cmd"
 * section rather than RAM. The entry is not referenced by name anywhere, so the
 * KEEP() in the linker fragment is what stops the linker from garbage-collecting
 * it.
 */
#define CMD_REGISTER(id, name_, fn_, help_)                       \
	static const STRUCT_SECTION_ITERABLE(cmd, _cmd_##id) = {  \
		.name = (name_),                                  \
		.fn = (fn_),                                      \
		.help = (help_),                                  \
	}

#endif /* CMD_H_ */
