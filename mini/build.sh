#!/usr/bin/env bash
#
# Build the bare-metal current-measurement firmwares (while1.hex, step.hex).
# Uses the Zephyr SDK's ARM toolchain - no Zephyr, no libc, no startup files.
#
#   Run from anywhere (Git Bash):  ./build.sh
#   Flash (from mini/):            pyocd flash -t pic32cm6408pl10048 -f 100000 step.hex
#
# IMPORTANT: linking is done with `ld` directly, NOT via gcc. The gcc link driver
# places the C .text at a default 0x10000000 (orphan) instead of into FLASH; ld
# with the script below puts everything at 0x0c000000 as intended.
set -e

# --- toolchain (adjust SDK_VER / TC if your SDK lives elsewhere) --------------
SDK_VER=1.0.1
TC="${ZEPHYR_SDK:-$HOME/zephyr-sdk-$SDK_VER}/gnu/arm-zephyr-eabi/bin"
GCC="$TC/arm-zephyr-eabi-gcc"
LD="$TC/arm-zephyr-eabi-ld"
OC="$TC/arm-zephyr-eabi-objcopy"

# --- SoC + CMSIS headers from the repo's pinned HAL module --------------------
HERE="$(cd "$(dirname "$0")" && pwd)"
M="$HERE/../modules"
INC="-I$M/hal/microchip/packs/pic32c/pic32cm_pl/pic32cm_pl10/include \
     -I$M/hal/cmsis_6/CMSIS/Core/Include \
     -I$M/hal/microchip/include"

ARCH="-mcpu=cortex-m0plus -mthumb"
CFLAGS="$ARCH -Os -ffreestanding -nostdlib -nostartfiles"

cd "$HERE"

echo "== while1 (pure while(1)) =="
"$GCC" $ARCH -c while1.s -o while1.o
"$LD" -T while1.ld while1.o -o while1.elf
"$OC" -O ihex while1.elf while1.hex

echo "== step (OSCHF -> OSC32K -> standby) =="
"$GCC" $ARCH -c step_vectors.s -o step_vectors.o
"$GCC" $CFLAGS $INC -c step.c -o step.o
"$LD" -T step.ld step_vectors.o step.o -o step.elf
"$OC" -O ihex step.elf step.hex

echo "OK -> while1.hex, step.hex"
