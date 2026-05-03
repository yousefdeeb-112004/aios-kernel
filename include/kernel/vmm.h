#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H
#include <kernel/types.h>
#define PTE_PRESENT 0x001
#define PTE_WRITABLE 0x002
#define PTE_USER 0x004
typedef struct { uint32_t pages_mapped; uint32_t page_faults; uint32_t maps_created; uint32_t maps_destroyed; uint32_t addr_spaces_created; uint32_t cr3_switches; } vmm_stats_t;
extern vmm_stats_t g_vmm_stats;
void vmm_init(void); void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_unmap_page(uint32_t virt); uint32_t vmm_get_physical(uint32_t virt); void vmm_dump(void);

/* Per-process address space support */
uint32_t vmm_create_address_space(void);   /* Allocate new page dir with kernel mappings cloned */
void     vmm_destroy_address_space(uint32_t page_dir_phys);
void     vmm_switch_address_space(uint32_t page_dir_phys);
uint32_t vmm_get_kernel_page_dir(void);    /* Get the kernel's page directory physical address */
#endif
