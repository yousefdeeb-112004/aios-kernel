#ifndef _KERNEL_HEAP_H
#define _KERNEL_HEAP_H
#include <kernel/types.h>
typedef struct { uint32_t alloc_count; uint32_t free_count; uint32_t total_allocated; uint32_t total_freed; uint32_t heap_size; uint32_t largest_free; uint32_t free_blocks; uint32_t coalesces; } heap_stats_t;
extern heap_stats_t g_heap_stats;
void heap_init(void); void* kmalloc(size_t size); void kfree(void* ptr);
void* kmalloc_aligned(size_t size); void heap_dump(void);
#endif
