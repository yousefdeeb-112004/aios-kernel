/* =============================================================================
 * User Mode (Ring 3) Support
 *
 * Ring 0 → Ring 3 transition lives in usermode_asm.S (jump_to_usermode).
 *
 * NOTE: The former usermode_demo()/`usr` command jumped to user_hello(), a
 * function compiled INTO the kernel image (a kernel .text address). Once real
 * supervisor/user isolation makes kernel .text supervisor-only (see vmm_init),
 * executing kernel code in Ring 3 faults on the instruction fetch — so that
 * demo can no longer work by construction. It was removed in favour of the
 * real path: `shgl <file.elf>` loads a genuine, separately-linked Ring 3
 * program into its own isolated address space (see elf_loader.c).
 * ============================================================================= */

#include <kernel/usermode.h>
