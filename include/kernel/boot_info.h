#ifndef _KERNEL_BOOT_INFO_H
#define _KERNEL_BOOT_INFO_H
#include <kernel/types.h>
typedef struct { uint32_t magic; uint32_t multiboot_valid; uint32_t total_memory_kb; uint32_t mem_lower_kb; uint32_t mem_upper_kb; uint32_t boot_device; uint32_t kernel_start; uint32_t kernel_end; uint32_t kernel_size_kb; uint32_t mmap_entries; uint32_t ai_features; } boot_info_t;
#define BOOT_INFO_MAGIC 0xA105B007
extern boot_info_t g_boot_info;
void boot_info_init(uint32_t multiboot_magic, void* multiboot_info_ptr);
void boot_info_dump(void);
#endif
