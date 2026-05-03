/* =============================================================================
 * User Mode (Ring 3) Support
 *
 * Provides the ability to run code in Ring 3 (unprivileged mode).
 * User programs communicate with the kernel only via INT 0x80 syscalls.
 * ============================================================================= */
#ifndef _KERNEL_USERMODE_H
#define _KERNEL_USERMODE_H

#include <kernel/types.h>

/* Jump to Ring 3. Does not return (user calls sys_exit to terminate).
 * eip = user code entry point
 * esp = user stack pointer (top of allocated user stack) */
extern void jump_to_usermode(uint32_t eip, uint32_t esp);

/* Create and run a demo user-mode program */
void usermode_demo(void);

#endif
