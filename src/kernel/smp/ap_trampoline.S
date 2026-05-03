/* =============================================================================
 * AP Trampoline Code
 *
 * This code is copied to physical address 0x8000 before sending SIPI.
 * Application Processors start executing here in 16-bit real mode.
 * It switches to 32-bit protected mode and calls the C entry point.
 *
 * Memory layout at 0x8000:
 *   0x8000: trampoline code (real mode entry)
 *   0x8FF0: AP stack pointer (set by BSP before SIPI)
 *   0x8FF4: AP entry point (set by BSP, points to C function)
 *   0x8FF8: AP GDT pointer (set by BSP)
 * ============================================================================= */

.section .text
.code16
.global ap_trampoline_start
.global ap_trampoline_end

ap_trampoline_start:
    /* We're running at 0x8000 in real mode */
    cli
    cld

    /* Set up real mode segments */
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* Load the GDT that BSP prepared for us */
    lgdt (0x8FF8)

    /* Enable protected mode */
    movl %cr0, %eax
    orl  $1, %eax
    movl %eax, %cr0

    /* Far jump to 32-bit code — selector 0x08 = kernel code */
    .byte 0x66, 0xEA          /* ljmp opcode for 32-bit target */
    .long 0x8000 + (pm_entry - ap_trampoline_start)
    .word 0x08

.code32
pm_entry:
    /* Now in 32-bit protected mode */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Load AP stack pointer from 0x8FF0 */
    movl 0x8FF0, %esp

    /* Call AP C entry point from 0x8FF4 */
    movl 0x8FF4, %eax
    call *%eax

    /* Should not return, but halt if it does */
    cli
    hlt
    jmp . - 2

ap_trampoline_end:
    nop

.section .note.GNU-stack, "", @progbits
