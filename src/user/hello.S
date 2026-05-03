/* Minimal ELF user program — Ring 3
 * Only uses INT 0x80 syscalls with immediate values.
 * NO self-modifying code. NO memory writes. */

.section .text
.global _start

_start:
    /* sys_write(buf=msg1, len) */
    movl $1, %eax
    movl $msg1, %ebx
    movl $msg1_len, %ecx
    int  $0x80

    /* sys_write(buf=msg2, len) */
    movl $1, %eax
    movl $msg2, %ebx
    movl $msg2_len, %ecx
    int  $0x80

    /* sys_getpid → EAX */
    movl $3, %eax
    int  $0x80
    /* ignore result, just prove it worked */

    /* sys_write(buf=msg3, len) */
    movl $1, %eax
    movl $msg3, %ebx
    movl $msg3_len, %ecx
    int  $0x80

    /* sys_exit */
    movl $4, %eax
    int  $0x80

    /* Never reached */
    hlt
    jmp .

msg1:
    .ascii "  ==========================================\n"
    .ascii "  |  ELF Program running in Ring 3!        |\n"
    .ascii "  |  Loaded by AIOS ELF Loader             |\n"
    .ascii "  ==========================================\n"
.set msg1_len, . - msg1

msg2:
    .ascii "  Code at 0x00500000. Syscalls via INT 0x80\n"
.set msg2_len, . - msg2

msg3:
    .ascii "  Goodbye from ELF user program!\n"
.set msg3_len, . - msg3

.section .note.GNU-stack, "", @progbits
