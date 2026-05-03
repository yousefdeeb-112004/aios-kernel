.section .data
gdt_start:
gdt_null: .long 0; .long 0
gdt_code: .word 0xFFFF; .word 0; .byte 0; .byte 0b10011010; .byte 0b11001111; .byte 0
gdt_data: .word 0xFFFF; .word 0; .byte 0; .byte 0b10010010; .byte 0b11001111; .byte 0
gdt_end:
gdt_descriptor: .word gdt_end - gdt_start - 1; .long gdt_start
.set CODE_SEG, gdt_code - gdt_start
.set DATA_SEG, gdt_data - gdt_start
.section .text
.global gdt_install
gdt_install:
    lgdt gdt_descriptor
    movw $DATA_SEG, %ax
    movw %ax, %ds; movw %ax, %es; movw %ax, %fs; movw %ax, %gs; movw %ax, %ss
    ljmp $CODE_SEG, $.flush_cs
.flush_cs: ret
.section .note.GNU-stack, "", @progbits
