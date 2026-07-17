ENTRY(reset_handler)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x0c000000, LENGTH = 64K
    RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
    } > FLASH

    /DISCARD/ :
    {
        *(.ARM.*)
        *(.comment)
    }
}
