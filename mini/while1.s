/* while1.s - the absolute-minimum bare-metal firmware for PIC32CM6408PL10048.
 *
 * Comes out of reset and spins forever in a busy while(1). NO init of anything -
 * the boot ROM leaves OSCHF running at its default, and the CPU just executes a
 * branch-to-self. Measures the raw board+CPU base current with zero software.
 *
 * Vector table (SP + reset vector) and the loop live in one .vectors section so
 * they sit contiguously at the flash base 0x0c000000.
 *
 * Measured on this board (POWER-Z KM003C, USB): ~16.2 mA - essentially all of it
 * the on-board nEDBG debugger + power LED + regulators (see mini/README.md).
 */
    .syntax unified
    .cpu cortex-m0plus
    .thumb

    .section .vectors, "ax", %progbits
    .global reset_handler
    .word 0x20002000            /* [0] initial SP = top of 8 KB SRAM */
    .word reset_handler + 1     /* [1] reset vector (thumb bit set) */

    .thumb_func
reset_handler:
    b   reset_handler           /* while (1) {} */
