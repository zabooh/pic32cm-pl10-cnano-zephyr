/* step_vectors.s - vector table + entry for the bare-metal step demo (step.c).
 * SP + reset vector at the flash base; reset jumps to the C routine c_main().
 * Kept in its own .vectors section that the linker places first at 0x0c000000.
 */
    .syntax unified
    .cpu cortex-m0plus
    .thumb

    .section .vectors, "ax", %progbits
    .global reset_handler
    .word 0x20002000            /* initial SP = top of 8 KB SRAM */
    .word reset_handler + 1     /* reset vector */

    .thumb_func
reset_handler:
    bl  c_main
    b   .
