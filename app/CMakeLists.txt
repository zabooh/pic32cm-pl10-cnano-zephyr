cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pic32cm_pl10_blinky_shell)
target_sources(app PRIVATE src/main.c src/led_ctrl.c src/pl10_adc.c src/cmd_parser.c src/fault.c src/diag.c src/button.c)

# Declare the "cmd" console-command registry as an iterable ROM section (see
# app/src/cmd.h and app/cmd_sections.ld). SECTIONS (not ROM_SECTIONS) because
# ITERABLE_SECTION_ROM already places itself into the ROMABLE_REGION.
zephyr_linker_sources(SECTIONS cmd_sections.ld)
