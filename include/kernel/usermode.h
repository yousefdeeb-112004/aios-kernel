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

/* User heap (SYS_SBRK), carved out of the same 4MB region.
 *
 * USER_HEAP_START sits 256 KB above the load base. The in-tree demo program
 * (src/user/hello.S) has a single PT_LOAD spanning [0x00500000, 0x00500145) —
 * one page — so 256 KB is generous headroom for a program image to grow before
 * it could ever collide with the heap. It is page-aligned, and it lives in the
 * same 4 MB page-directory slot (PDI 1) as the code and the stack, which
 * vmm_map_user_page requires (it backs exactly one PDE per address space).
 *
 * USER_HEAP_MAX is the EXCLUSIVE upper bound of the break. The user stack owns
 * the top page, [USER_STACK_TOP-4096, USER_STACK_TOP) = [0x005FF000,0x00600000),
 * and the page directly below it, [0x005FE000, 0x005FF000), is left permanently
 * unmapped as a GUARD PAGE. The heap therefore can never grow into the stack
 * silently: sbrk refuses at USER_HEAP_MAX, and a runaway stack faults into
 * unmapped memory instead of scribbling on the heap.
 *
 * Usable heap span: [0x00540000, 0x005FE000) = 760 KB. */
#define USER_HEAP_START    0x00540000u   /* page-aligned, 256KB above the image */
#define USER_HEAP_MAX      0x005FE000u   /* exclusive; == guard page base       */
#define USER_GUARD_PAGE    USER_HEAP_MAX /* [0x005FE000,0x005FF000) never mapped */

/* Jump to Ring 3. Does not return (user calls sys_exit to terminate).
 * eip = user code entry point
 * esp = user stack pointer (top of allocated user stack) */
extern void jump_to_usermode(uint32_t eip, uint32_t esp);

#endif
