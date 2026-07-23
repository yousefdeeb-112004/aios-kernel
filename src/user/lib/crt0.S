/* =============================================================================
 * libaios — C runtime startup for Ring 3 programs
 *
 * The ELF loader enters here at e_entry (ENTRY(_start) in src/user/user.ld)
 * with ESP = USER_STACK_TOP - 16. There are no argc/argv/envp: the AIOS ABI
 * has no notion of program arguments, so main() takes none.
 *
 * .bss does not need clearing here — src/user/user.ld folds .bss into the same
 * PROGBITS output section as .text/.data, so the zeros are in the file image
 * and the ELF loader copies them in with the rest of the segment.
 * ========================================================================== */

.section .text
.global _start
.extern main

_start:
    /* Terminate the frame-pointer chain so any future stack walker stops
     * here instead of chasing whatever the loader left in EBP. */
    xorl %ebp, %ebp

    call main

    /* ABI v1: SYS_EXIT takes NO arguments — there is no exit status, so
     * main's return value in EAX is deliberately discarded. Do not invent an
     * argument here; the kernel would ignore it and the ABI is frozen. */
    movl $4, %eax          /* SYS_EXIT */
    int  $0x80

    /* Unreachable: SYS_EXIT never returns. Park the CPU if it somehow does. */
1:  jmp 1b

.section .note.GNU-stack, "", @progbits
