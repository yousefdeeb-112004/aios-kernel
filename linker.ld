ENTRY(_start)
SECTIONS
{
    . = 1M;
    _kernel_start = .;
    .multiboot ALIGN(4) : { *(.multiboot) }
    .text ALIGN(4K) : { *(.text) }
    .rodata ALIGN(4K) : { *(.rodata) *(.rodata.*) }
    .data ALIGN(4K) : { *(.data) }
    .bss ALIGN(4K) : { _bss_start = .; *(COMMON) *(.bss) _bss_end = .; }
    _kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
