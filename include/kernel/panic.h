/* =============================================================================
 * Kernel Panic — Full Register Dump + Stack Trace
 *
 * Replaces the simple "!!! PANIC: GPF EIP:0x..." with a full-screen
 * panic display showing all registers, faulting address, error code,
 * stack contents, and a call trace (frame pointer walk).
 *
 * Also provides kpanic() for use anywhere in kernel code.
 * ============================================================================= */
#ifndef _KERNEL_PANIC_H
#define _KERNEL_PANIC_H

#include <kernel/types.h>
#include <kernel/idt.h>

/* Trigger kernel panic with message. Never returns. */
void kpanic(const char* message);

/* Trigger kernel panic with full register dump from ISR frame. */
void kpanic_exception(registers_t* regs);

#endif
