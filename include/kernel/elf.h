/* =============================================================================
 * ELF Format Definitions + Loader API
 *
 * Supports loading 32-bit ELF executables from VFS into memory
 * and executing them in Ring 3.
 *
 * Shell command: shgl (شغّل = run/execute)
 * ============================================================================= */
#ifndef _KERNEL_ELF_H
#define _KERNEL_ELF_H

#include <kernel/types.h>

/* ELF Magic */
#define ELF_MAGIC  0x464C457F   /* "\x7FELF" as uint32 */

/* ELF header (32-bit) */
typedef struct {
    uint32_t e_magic;       /* 0x7F 'E' 'L' 'F' */
    uint8_t  e_class;       /* 1 = 32-bit */
    uint8_t  e_data;        /* 1 = little-endian */
    uint8_t  e_version;     /* 1 = current */
    uint8_t  e_osabi;
    uint8_t  e_pad[8];
    uint16_t e_type;        /* 2 = ET_EXEC */
    uint16_t e_machine;     /* 3 = EM_386 */
    uint32_t e_version2;
    uint32_t e_entry;       /* Entry point virtual address */
    uint32_t e_phoff;       /* Program header table offset */
    uint32_t e_shoff;       /* Section header table offset */
    uint32_t e_flags;
    uint16_t e_ehsize;      /* ELF header size */
    uint16_t e_phentsize;   /* Program header entry size */
    uint16_t e_phnum;       /* Number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_header_t;

/* Program header (32-bit) */
typedef struct {
    uint32_t p_type;        /* 1 = PT_LOAD */
    uint32_t p_offset;      /* Offset in file */
    uint32_t p_vaddr;       /* Virtual address to load at */
    uint32_t p_paddr;       /* Physical address (unused) */
    uint32_t p_filesz;      /* Size in file */
    uint32_t p_memsz;       /* Size in memory (may be > filesz for BSS) */
    uint32_t p_flags;       /* PF_R=4, PF_W=2, PF_X=1 */
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr_t;

/* ELF constants */
#define ET_EXEC     2       /* Executable file */
#define EM_386      3       /* Intel 80386 */
#define PT_LOAD     1       /* Loadable segment */

/* Load and execute an ELF binary from VFS.
 * Returns 0 on success, -1 on failure. */
int32_t elf_load_and_run(const char* filename);

/* Validate an ELF header. Returns true if valid 32-bit x86 ELF. */
bool elf_validate(const elf32_header_t* hdr);

/* Ring-3 isolation self-test: run a tiny Ring 3 program that stores to a
 * kernel address; it must be killed with [SEGFAULT] while the shell survives. */
int32_t elf_run_segfault_test(void);

/* Ring-3 syscall pointer-guard self-test: a Ring 3 SYS_WRITE with a kernel
 * address must return -1 (nothing leaked) while the program keeps running. */
int32_t elf_run_syscall_guard_test(void);

/* Ring-3 sbrk self-test: a Ring 3 program grows its heap with SYS_SBRK, writes
 * and reads back both ends of the allocation, and checks that an over-limit
 * request is refused with -1. Reports PMM free_pages before/after. */
int32_t elf_run_sbrk_test(void);

/* Ring-3 file-access self-test: a Ring 3 program opens a VFS file, reads and
 * hex-dumps its first 16 bytes, closes it, then checks that SYS_OPEN with a
 * kernel-address path is rejected. Reports VFS open_fds before/after so the
 * descriptor it deliberately leaks can be seen to be reclaimed on exit. */
int32_t elf_run_file_test(void);

#endif
