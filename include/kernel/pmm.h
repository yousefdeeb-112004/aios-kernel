#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H
#include <kernel/types.h>
#define PAGE_SIZE 4096
typedef struct { uint32_t total_pages; uint32_t used_pages; uint32_t free_pages; uint32_t peak_used; uint32_t alloc_count; uint32_t free_count; } pmm_stats_t;
extern pmm_stats_t g_pmm_stats;
void pmm_init(uint32_t mmap_addr, uint32_t mmap_length, uint32_t kernel_start, uint32_t kernel_end);
uint32_t pmm_alloc_page(void); void pmm_free_page(uint32_t phys_addr);
bool pmm_is_page_used(uint32_t phys_addr); void pmm_dump(void);
#endif
