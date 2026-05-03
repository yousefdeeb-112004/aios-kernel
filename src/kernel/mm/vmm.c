/* VMM — Virtual Memory Manager with per-process address spaces
 *
 * The kernel page directory identity-maps the first 32MB (8 page tables).
 * Each new process gets its own page directory with kernel mappings cloned.
 * CR3 is switched during context switches. */

#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/idt.h>
#include <kernel/panic.h>
#include <drivers/vga.h>
#include <lib/string.h>

static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t first_pt[8][1024] __attribute__((aligned(4096)));
vmm_stats_t g_vmm_stats;

static void pf(registers_t* r) { g_vmm_stats.page_faults++; kpanic_exception(r); }

void vmm_init(void) {
    memset(&g_vmm_stats, 0, sizeof(vmm_stats_t));
    memset(page_dir, 0, sizeof(page_dir));

    /* Identity-map first 32MB (8 page tables × 1024 pages × 4KB = 32MB) */
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < 1024; i++) {
            first_pt[t][i] = (t * 1024 + i) * PAGE_SIZE | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            g_vmm_stats.pages_mapped++;
        }
        page_dir[t] = (uint32_t)&first_pt[t] | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    g_vmm_stats.maps_created = g_vmm_stats.pages_mapped;
    idt_register_handler(14, pf);

    /* Enable paging */
    __asm__ volatile("mov %0, %%cr3" :: "r"(page_dir));
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
}

void vmm_map_page(uint32_t v, uint32_t p, uint32_t f) {
    uint32_t pdi = v >> 22;
    uint32_t pti = (v >> 12) & 0x3FF;
    if (!(page_dir[pdi] & PTE_PRESENT)) {
        uint32_t pt = pmm_alloc_page();
        if (!pt) return;
        memset((void*)pt, 0, PAGE_SIZE);
        page_dir[pdi] = pt | PTE_PRESENT | PTE_WRITABLE | (f & PTE_USER);
    }
    uint32_t* pt = (uint32_t*)(page_dir[pdi] & 0xFFFFF000);
    pt[pti] = p | (f & 0xFFF) | PTE_PRESENT;
    __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
    g_vmm_stats.pages_mapped++;
    g_vmm_stats.maps_created++;
}

void vmm_unmap_page(uint32_t v) {
    uint32_t pdi = v >> 22;
    uint32_t pti = (v >> 12) & 0x3FF;
    if (!(page_dir[pdi] & PTE_PRESENT)) return;
    uint32_t* pt = (uint32_t*)(page_dir[pdi] & 0xFFFFF000);
    pt[pti] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
    g_vmm_stats.pages_mapped--;
    g_vmm_stats.maps_destroyed++;
}

uint32_t vmm_get_physical(uint32_t v) {
    uint32_t pdi = v >> 22;
    uint32_t pti = (v >> 12) & 0x3FF;
    if (!(page_dir[pdi] & PTE_PRESENT)) return 0;
    uint32_t* pt = (uint32_t*)(page_dir[pdi] & 0xFFFFF000);
    if (!(pt[pti] & PTE_PRESENT)) return 0;
    return (pt[pti] & 0xFFFFF000) | (v & 0xFFF);
}

uint32_t vmm_get_kernel_page_dir(void) {
    return (uint32_t)page_dir;
}

/* Create a new address space for user-mode programs.
 * Allocates from the kernel heap (guaranteed within identity-mapped 32MB)
 * since pmm_alloc_page() could return pages above 32MB which aren't mapped.
 * Returns physical address of the new page directory. */
uint32_t vmm_create_address_space(void) {
    /* Allocate page-aligned memory from heap (within identity-mapped region) */
    uint32_t* new_pd = (uint32_t*)kmalloc(PAGE_SIZE + PAGE_SIZE);
    if (!new_pd) return 0;

    /* Align to page boundary */
    uint32_t addr = (uint32_t)new_pd;
    uint32_t aligned = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t* pd = (uint32_t*)aligned;
    memset(pd, 0, PAGE_SIZE);

    /* Clone kernel space: copy the first 8 page directory entries (first 32MB).
     * These point to the SAME page tables, so kernel memory is shared. */
    for (int i = 0; i < 8; i++) {
        pd[i] = page_dir[i];
    }

    /* Also clone any other kernel mappings (LAPIC at 0xFEE00000, etc.) */
    for (int i = 8; i < 1024; i++) {
        if (page_dir[i] & PTE_PRESENT) {
            pd[i] = page_dir[i];
        }
    }

    g_vmm_stats.addr_spaces_created++;
    return (uint32_t)pd;
}

/* Destroy an address space. For heap-allocated page directories,
 * the memory will be freed when the process's heap is cleaned up.
 * We just free any process-specific page tables. */
void vmm_destroy_address_space(uint32_t page_dir_phys) {
    if (page_dir_phys == 0 || page_dir_phys == (uint32_t)page_dir) return;

    uint32_t* pd = (uint32_t*)page_dir_phys;

    /* Free any process-specific page tables (above kernel range) */
    for (int i = 8; i < 1024; i++) {
        if (pd[i] & PTE_PRESENT) {
            uint32_t pt_phys = pd[i] & 0xFFFFF000;
            if ((page_dir[i] & 0xFFFFF000) != pt_phys) {
                pmm_free_page(pt_phys);
            }
        }
    }
    /* Note: the page directory itself was allocated with kmalloc,
     * it gets freed via kfree in proc_kill */
}

/* Switch to a different address space by loading its page directory into CR3 */
void vmm_switch_address_space(uint32_t page_dir_phys) {
    if (page_dir_phys == 0) return;
    __asm__ volatile("mov %0, %%cr3" :: "r"(page_dir_phys) : "memory");
    g_vmm_stats.cr3_switches++;
}

void vmm_dump(void) {
    vga_puts("  VMM: ");
    vga_put_dec(g_vmm_stats.pages_mapped); vga_puts(" mapped, ");
    vga_put_dec(g_vmm_stats.page_faults); vga_puts(" faults, ");
    vga_put_dec(g_vmm_stats.addr_spaces_created); vga_puts(" addr spaces, ");
    vga_put_dec(g_vmm_stats.cr3_switches); vga_puts(" CR3 switches\n");
}
