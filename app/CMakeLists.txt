cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pic32cm_pl10_blinky_shell)
target_sources(app PRIVATE src/main.c src/pl10_adc.c src/cmd_parser.c)
