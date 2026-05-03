#ifndef _KERNEL_MULTIBOOT_H
#define _KERNEL_MULTIBOOT_H
#include <kernel/types.h>
#define MULTIBOOT_MAGIC 0x2BADB002
#define MULTIBOOT_FLAG_MEM 0x001
#define MULTIBOOT_FLAG_BOOTDEV 0x002
#define MULTIBOOT_FLAG_MMAP 0x040
typedef struct { uint32_t flags; uint32_t mem_lower; uint32_t mem_upper; uint32_t boot_device; uint32_t cmdline; uint32_t mods_count; uint32_t mods_addr; uint32_t syms[4]; uint32_t mmap_length; uint32_t mmap_addr; uint32_t drives_length; uint32_t drives_addr; uint32_t config_table; uint32_t boot_loader_name; uint32_t apm_table; } __attribute__((packed)) multiboot_info_t;
typedef struct { uint32_t size; uint64_t base_addr; uint64_t length; uint32_t type; } __attribute__((packed)) multiboot_mmap_entry_t;
#define MMAP_TYPE_AVAILABLE 1
#define MMAP_TYPE_RESERVED 2
#define MMAP_TYPE_ACPI_RECLAIM 3
#endif
