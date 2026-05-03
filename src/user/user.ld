/* User program linker script — single LOAD segment at 0x00500000 */
ENTRY(_start)
SECTIONS
{
    . = 0x00500000;

    .all : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
        *(.bss)
        *(.bss.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
