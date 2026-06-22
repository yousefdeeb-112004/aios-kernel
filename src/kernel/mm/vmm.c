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

    /* Identity-map first 32MB (8 page tables × 1024 pages × 4KB = 32MB).
     * These are kernel pages: SUPERVISOR-ONLY (no PTE_USER). Ring 3 code
     * therefore cannot read or write the kernel .text/.rodata/.data/.bss or
     * any kernel working memory. User ELF programs get their own user-flagged
     * pages in a private address space (see vmm_map_user_page). */
    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < 1024; i++) {
            first_pt[t][i] = (t * 1024 + i) * PAGE_SIZE | PTE_PRESENT | PTE_WRITABLE;
            g_vmm_stats.pages_mapped++;
        }
        page_dir[t] = (uint32_t)&first_pt[t] | PTE_PRESENT | PTE_WRITABLE;
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

/* ---- Isolated user address spaces ----------------------------------------
 * User ELF programs run under their own page directory. The directory and its
 * one private page table are drawn from a static, IDENTITY-MAPPED pool in the
 * kernel image (BSS lives below the heap and is never remapped, so each
 * structure's virtual address equals its physical address) — exactly what CR3
 * and page-directory entries must contain. The kernel heap is NOT
 * identity-mapped (heap_init remaps it onto arbitrary PMM frames), so kmalloc
 * memory cannot be used to hold page tables.
 *
 * KNOWN TRAP: the user load region (~0x00500000) lives inside the kernel's
 * shared page table first_pt[1], which is now supervisor-only. Punching a user
 * page straight into first_pt[1] would re-expose kernel memory to Ring 3 in
 * every address space (the table is shared). Instead each address space gets a
 * PRIVATE copy of that 4MB region's page table: the kernel PTEs are copied
 * verbatim (PTE_USER clear → still supervisor) and only the program's own PTEs
 * get PTE_USER. The PDE gets PTE_USER set so those user PTEs are reachable (a
 * page is user-accessible only when USER is set at BOTH levels, so the copied
 * kernel PTEs stay supervisor regardless).
 *
 * This kernel loads every user program into the single 4MB region at
 * 0x00500000 (one page-directory slot, see user.ld), so one private page table
 * per address space is enough. */
#define MAX_USER_AS 4

static uint32_t user_pd_pool[MAX_USER_AS][1024] __attribute__((aligned(4096)));
static uint32_t user_pt_pool[MAX_USER_AS][1024] __attribute__((aligned(4096)));
static bool     user_as_used[MAX_USER_AS];
static uint32_t user_as_pdi[MAX_USER_AS];   /* PDE that the private table backs */

static int user_as_slot(uint32_t pd_phys) {
    for (int i = 0; i < MAX_USER_AS; i++)
        if (user_as_used[i] && (uint32_t)user_pd_pool[i] == pd_phys) return i;
    return -1;
}

/* Create a new user address space cloned from the kernel's. Kernel page tables
 * are shared and stay supervisor-only (PTE_USER was cleared in vmm_init), so
 * Ring 3 cannot reach kernel memory even though the mappings remain present for
 * Ring-0 syscall/IRQ handling under this CR3. Returns the page directory's
 * physical address (== its virtual address; the pool is identity-mapped). */
uint32_t vmm_create_address_space(void) {
    int slot = -1;
    for (int i = 0; i < MAX_USER_AS; i++)
        if (!user_as_used[i]) { slot = i; break; }
    if (slot < 0) return 0;   /* pool exhausted */

    uint32_t* pd = user_pd_pool[slot];
    memcpy(pd, page_dir, PAGE_SIZE);   /* clone all kernel mappings */

    user_as_used[slot] = true;
    user_as_pdi[slot]  = 0xFFFFFFFF;
    g_vmm_stats.addr_spaces_created++;
    return (uint32_t)pd;
}

/* Map [virt -> phys] as a RING-3 (user-accessible) page into the address space
 * whose page directory is at pd_phys. */
void vmm_map_user_page(uint32_t pd_phys, uint32_t virt, uint32_t phys) {
    int slot = user_as_slot(pd_phys);
    if (slot < 0) return;
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FF;
    uint32_t* pd = user_pd_pool[slot];
    uint32_t* pt = user_pt_pool[slot];

    if (user_as_pdi[slot] == 0xFFFFFFFF) {
        /* First user page: seed the private table from the kernel's table for
         * this 4MB region (keeps the rest supervisor), then splice it in with
         * the PDE user bit set. */
        if (page_dir[pdi] & PTE_PRESENT)
            memcpy(pt, (void*)(page_dir[pdi] & 0xFFFFF000), PAGE_SIZE);
        else
            memset(pt, 0, PAGE_SIZE);
        pd[pdi] = (uint32_t)pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        user_as_pdi[slot] = pdi;
    } else if (user_as_pdi[slot] != pdi) {
        /* This kernel confines user pages to one 4MB region; refuse others. */
        return;
    }

    pt[pti] = (phys & 0xFFFFF000) | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* Destroy a user address space: free the PMM frames backing the program (the
 * PTE_USER entries we punched in; copied kernel PTEs are supervisor and are
 * left alone) and return the pool slot. */
void vmm_destroy_address_space(uint32_t pd_phys) {
    if (pd_phys == 0 || pd_phys == (uint32_t)page_dir) return;
    int slot = user_as_slot(pd_phys);
    if (slot < 0) return;

    if (user_as_pdi[slot] != 0xFFFFFFFF) {
        uint32_t* pt = user_pt_pool[slot];
        for (int j = 0; j < 1024; j++)
            if ((pt[j] & PTE_PRESENT) && (pt[j] & PTE_USER))
                pmm_free_page(pt[j] & 0xFFFFF000);
    }
    user_as_used[slot] = false;
    user_as_pdi[slot]  = 0xFFFFFFFF;
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
