/* =============================================================================
 * GDT + TSS for User Mode (Ring 3)
 *
 * GDT Layout:
 *   0x00 (0)  Null
 *   0x08 (1)  Kernel Code  DPL=0
 *   0x10 (2)  Kernel Data  DPL=0
 *   0x18 (3)  User Code    DPL=3
 *   0x20 (4)  User Data    DPL=3
 *   0x28 (5)  TSS
 *
 * Selectors:
 *   Kernel CS = 0x08    Kernel DS = 0x10
 *   User CS   = 0x1B    User DS   = 0x23   (0x18|3, 0x20|3)
 *   TSS       = 0x28
 * ============================================================================= */
#ifndef _KERNEL_GDT_H
#define _KERNEL_GDT_H

#include <kernel/types.h>

#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x18
#define GDT_USER_DATA    0x20
#define GDT_TSS_SEG      0x28

/* User-mode selectors include RPL=3 */
#define USER_CODE_SEL    (GDT_USER_CODE | 3)   /* 0x1B */
#define USER_DATA_SEL    (GDT_USER_DATA | 3)   /* 0x23 */

/* TSS structure (Intel-defined, 104 bytes) */
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;       /* Kernel stack pointer (loaded on Ring 3->0 transition) */
    uint32_t ss0;        /* Kernel stack segment (0x10) */
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

/* Initialize full GDT with user segments and TSS */
void gdt_init_full(void);

/* Set kernel stack in TSS (call before jumping to Ring 3) */
void tss_set_kernel_stack(uint32_t esp0);

#endif
