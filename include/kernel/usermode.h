/* =============================================================================
 * User Mode (Ring 3) Support
 *
 * Provides the ability to run code in Ring 3 (unprivileged mode).
 * User programs communicate with the kernel only via INT 0x80 syscalls.
 * ============================================================================= */
#ifndef _KERNEL_USERMODE_H
#define _KERNEL_USERMODE_H

#include <kernel/types.h>

/* User address-space layout (Ring 3), shared by the ELF loader and the syscall
 * pointer-validation code. Every user program is loaded into the single 4MB
 * region starting at USER_REGION_START (see src/user/user.ld); its stack sits
 * just below USER_STACK_TOP. Any pointer passed from Ring 3 to a syscall must
 * lie entirely within [USER_REGION_START, USER_STACK_TOP). */
#define USER_REGION_START  0x00500000u   /* ELF load base (src/user/user.ld)   */
#define USER_STACK_TOP     0x00600000u   /* top of user stack == region end    */
#define USER_REGION_END    USER_STACK_TOP

/* Jump to Ring 3. Does not return (user calls sys_exit to terminate).
 * eip = user code entry point
 * esp = user stack pointer (top of allocated user stack) */
extern void jump_to_usermode(uint32_t eip, uint32_t esp);

#endif
