.set MBOOT_MAGIC, 0x1BADB002
.set MBOOT_FLAGS, 0x00000003
.set MBOOT_CHECKSUM, -(MBOOT_MAGIC + MBOOT_FLAGS)
.section .multiboot, "a"
.align 4
    .long MBOOT_MAGIC
    .long MBOOT_FLAGS
    .long MBOOT_CHECKSUM
.section .bss
.align 16
stack_bottom: .skip 16384
stack_top:
.section .text
.global _start
.extern kmain
.extern gdt_install
_start:
    movl $stack_top, %esp
    pushl %ebx
    pushl %eax
    call gdt_install
    call kmain
.hang: cli; hlt; jmp .hang
.section .note.GNU-stack, "", @progbits
