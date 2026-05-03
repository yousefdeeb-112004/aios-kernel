/* =============================================================================
 * Full GDT with User Mode Segments + TSS
 *
 * Called from kmain after the boot GDT (gdt.S) has gotten us into C code.
 * This replaces the boot GDT with one that includes Ring 3 segments and TSS.
 * ============================================================================= */

#include <kernel/gdt.h>
#include <kernel/ports.h>
#include <lib/string.h>
#include <drivers/vga.h>

/* GDT entry structure */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* GDT pointer */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

#define GDT_ENTRIES 6

static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdt_ptr;
static tss_t       kernel_tss;

/* Set a GDT entry */
static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran) {
    gdt[idx].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[idx].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[idx].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt[idx].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[idx].granularity  = (uint8_t)((gran & 0xF0) | ((limit >> 16) & 0x0F));
    gdt[idx].access      = access;
}

void gdt_init_full(void) {
    memset(&kernel_tss, 0, sizeof(tss_t));

    /* TSS: set kernel stack segment and I/O map base */
    kernel_tss.ss0 = GDT_KERNEL_DATA;   /* 0x10 */
    kernel_tss.esp0 = 0;                 /* Set later before Ring 3 jump */
    kernel_tss.iomap_base = sizeof(tss_t);

    /* Entry 0: Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Entry 1 (0x08): Kernel Code — DPL=0, Execute/Read */
    /*   access: P=1 DPL=00 S=1 Type=1010 = 0x9A */
    /*   gran:   G=1 D=1 0 0  Limit=F    = 0xCF */
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* Entry 2 (0x10): Kernel Data — DPL=0, Read/Write */
    /*   access: P=1 DPL=00 S=1 Type=0010 = 0x92 */
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* Entry 3 (0x18): User Code — DPL=3, Execute/Read */
    /*   access: P=1 DPL=11 S=1 Type=1010 = 0xFA */
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /* Entry 4 (0x20): User Data — DPL=3, Read/Write */
    /*   access: P=1 DPL=11 S=1 Type=0010 = 0xF2 */
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* Entry 5 (0x28): TSS descriptor */
    /*   access: P=1 DPL=0 S=0 Type=1001 = 0x89 (available 32-bit TSS) */
    /*   gran:   G=0 (byte granularity), limit = sizeof(tss_t) - 1 */
    uint32_t tss_base = (uint32_t)&kernel_tss;
    uint32_t tss_limit = sizeof(tss_t) - 1;
    gdt_set_entry(5, tss_base, tss_limit, 0x89, 0x00);

    /* Load the new GDT */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint32_t)&gdt;
    __asm__ volatile("lgdt (%0)" : : "r"(&gdt_ptr));

    /* Reload segment registers with kernel selectors */
    __asm__ volatile(
        "movw $0x10, %%ax \n"
        "movw %%ax, %%ds  \n"
        "movw %%ax, %%es  \n"
        "movw %%ax, %%fs  \n"
        "movw %%ax, %%gs  \n"
        "movw %%ax, %%ss  \n"
        "ljmp $0x08, $1f  \n"   /* Far jump to reload CS */
        "1:                \n"
        : : : "eax"
    );

    /* Load TSS into Task Register */
    __asm__ volatile(
        "movw $0x28, %%ax \n"
        "ltr %%ax         \n"
        : : : "eax"
    );
}

void tss_set_kernel_stack(uint32_t esp0) {
    kernel_tss.esp0 = esp0;
}
