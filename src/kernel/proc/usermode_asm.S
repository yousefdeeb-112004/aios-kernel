/* =============================================================================
 * jump_to_usermode(uint32_t eip, uint32_t esp)
 *
 * Switches from Ring 0 to Ring 3 using IRET.
 * Sets up the stack so IRET pops: EIP, CS(user), EFLAGS, ESP(user), SS(user)
 *
 * After IRET, CPU is in Ring 3 with user code/data segments.
 * ============================================================================= */

.section .text
.global jump_to_usermode

jump_to_usermode:
    movl 4(%esp), %ecx      /* ECX = user EIP */
    movl 8(%esp), %edx      /* EDX = user ESP */

    /* Set data segment registers to user data selector (0x23 = 0x20|RPL3) */
    movw $0x23, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    /* Note: SS will be set by IRET */

    /* Build IRET frame on current kernel stack:
     *   [SS]      = 0x23 (user data)
     *   [ESP]     = user stack pointer
     *   [EFLAGS]  = current flags with IF=1 (interrupts enabled)
     *   [CS]      = 0x1B (user code = 0x18|RPL3)
     *   [EIP]     = user entry point
     */
    pushl $0x23             /* User SS */
    pushl %edx              /* User ESP */
    pushf                   /* EFLAGS */
    orl $0x200, (%esp)      /* Ensure IF (interrupt flag) is set */
    pushl $0x1B             /* User CS */
    pushl %ecx              /* User EIP */

    /* IRET: pops EIP, CS, EFLAGS, ESP, SS — switches to Ring 3 */
    iret

.section .note.GNU-stack, "", @progbits
