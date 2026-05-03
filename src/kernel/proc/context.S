.section .text
.global switch_context
switch_context:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    pushl %ebp; pushl %ebx; pushl %esi; pushl %edi
    movl %esp, (%eax)
    movl %ecx, %esp
    popl %edi; popl %esi; popl %ebx; popl %ebp
    ret
.section .note.GNU-stack, "", @progbits
