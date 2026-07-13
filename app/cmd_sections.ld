/*
 * Declares the "cmd" console-command registry as an iterable section in ROM
 * (flash/rodata). Emits the output section cmd_area plus the boundary symbols
 * _cmd_list_start / _cmd_list_end that STRUCT_SECTION_FOREACH(cmd, ...) walks.
 * Pulled into the linker script via zephyr_linker_sources(SECTIONS ...) in
 * app/CMakeLists.txt. See app/src/cmd.h for the C side.
 */
#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(cmd, Z_LINK_ITERABLE_SUBALIGN)
