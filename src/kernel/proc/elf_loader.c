/* =============================================================================
 * ELF Loader
 *
 * Loads 32-bit ELF executables from the VFS and runs them in Ring 3.
 *
 * Flow:
 *   1. Open file from VFS
 *   2. Read entire file into buffer
 *   3. Validate ELF header (magic, class, machine)
 *   4. Walk program headers, copy PT_LOAD segments to target addresses
 *   5. Allocate user stack
 *   6. Set TSS kernel stack
 *   7. IRET to Ring 3 at ELF entry point
 * ============================================================================= */

#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/gdt.h>
#include <kernel/usermode.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <lib/string.h>

#define USER_STACK_SIZE 4096

bool elf_validate(const elf32_header_t* hdr) {
    if (hdr->e_magic != ELF_MAGIC) {
        vga_puts_color("  ELF: bad magic\n", VGA_LIGHT_RED, VGA_BLACK);
        return false;
    }
    if (hdr->e_class != 1) {  /* Must be 32-bit */
        vga_puts_color("  ELF: not 32-bit\n", VGA_LIGHT_RED, VGA_BLACK);
        return false;
    }
    if (hdr->e_data != 1) {   /* Must be little-endian */
        vga_puts_color("  ELF: not little-endian\n", VGA_LIGHT_RED, VGA_BLACK);
        return false;
    }
    if (hdr->e_type != ET_EXEC) {
        vga_puts_color("  ELF: not executable\n", VGA_LIGHT_RED, VGA_BLACK);
        return false;
    }
    if (hdr->e_machine != EM_386) {
        vga_puts_color("  ELF: not i386\n", VGA_LIGHT_RED, VGA_BLACK);
        return false;
    }
    return true;
}

/* Kernel thread that loads and runs the ELF in Ring 3 */
static char elf_filename_buf[64];

static void elf_thread_fn(void) {
    const char* filename = elf_filename_buf;

    /* Get file size directly — no tmp buffer needed */
    int32_t file_size = vfs_filesize(filename);
    if (file_size < (int32_t)sizeof(elf32_header_t)) {
        vga_puts_color("  ELF: file not found or too small: ", VGA_LIGHT_RED, VGA_BLACK);
        vga_puts(filename);
        vga_puts("\n");
        return;
    }

    /* Allocate buffer and read entire file */
    uint8_t* file_buf = (uint8_t*)kmalloc(file_size);
    if (!file_buf) {
        vga_puts_color("  ELF: out of memory\n", VGA_LIGHT_RED, VGA_BLACK);
        return;
    }

    int32_t fd = vfs_open(filename);
    if (fd < 0) { kfree(file_buf); return; }

    uint32_t total_read = 0;
    int32_t n;
    while (total_read < (uint32_t)file_size) {
        n = vfs_read(fd, file_buf + total_read, file_size - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    vfs_close(fd);

    /* Validate ELF header */
    elf32_header_t* hdr = (elf32_header_t*)file_buf;
    if (!elf_validate(hdr)) {
        kfree(file_buf);
        return;
    }

    vga_puts("  ELF: valid i386 executable\n");
    vga_puts("  Entry: ");
    vga_put_hex(hdr->e_entry);
    vga_puts("  Segments: ");
    vga_put_dec(hdr->e_phnum);
    vga_puts("\n");

    /* Load PT_LOAD segments */
    uint32_t segments_loaded = 0;
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        elf32_phdr_t* ph = (elf32_phdr_t*)(file_buf + hdr->e_phoff +
                                             i * hdr->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;

        vga_puts("  Loading segment ");
        vga_put_dec(i);
        vga_puts(" -> ");
        vga_put_hex(ph->p_vaddr);
        vga_puts(" (");
        vga_put_dec(ph->p_filesz);
        vga_puts(" bytes)\n");

        /* Bounds check: must be within our identity-mapped region (0-16MB) */
        if (ph->p_vaddr + ph->p_memsz > 0x02000000) {
            vga_puts_color("  ELF: segment out of range!\n", VGA_LIGHT_RED, VGA_BLACK);
            kfree(file_buf);
            return;
        }

        /* Copy file data to target address */
        memcpy((void*)ph->p_vaddr, file_buf + ph->p_offset, ph->p_filesz);

        /* Zero BSS (memsz > filesz) */
        if (ph->p_memsz > ph->p_filesz) {
            memset((void*)(ph->p_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
        }

        segments_loaded++;
    }

    if (segments_loaded == 0) {
        vga_puts_color("  ELF: no loadable segments\n", VGA_LIGHT_RED, VGA_BLACK);
        kfree(file_buf);
        return;
    }

    vga_puts_color("  Jumping to Ring 3...\n", VGA_LIGHT_GREEN, VGA_BLACK);

    /* Save entry point BEFORE freeing — hdr points into file_buf! */
    uint32_t entry_point = hdr->e_entry;

    /* Free file buffer — segments are now copied to target addresses */
    kfree(file_buf);

    /* Allocate user stack */
    void* ustack = kmalloc(USER_STACK_SIZE);
    if (!ustack) {
        vga_puts_color("  ELF: stack alloc failed\n", VGA_LIGHT_RED, VGA_BLACK);
        return;
    }
    uint32_t user_esp = (uint32_t)ustack + USER_STACK_SIZE - 16;

    /* Track user stack for cleanup on process exit */
    proc_set_user_stack((uint32_t)ustack);
    uint32_t kstack_top = current_process->stack_base + PROC_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);

    /* Jump to Ring 3 at ELF entry point! */
    jump_to_usermode(entry_point, user_esp);
}

int32_t elf_load_and_run(const char* filename) {
    vga_puts_color("=== ELF Loader (shgl) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Loading: ");
    vga_puts(filename);
    vga_puts("\n");

    elf_filename_buf[0] = '\0';
    /* Safe copy */
    uint32_t flen = strlen(filename);
    if (flen > 62) flen = 62;
    memcpy(elf_filename_buf, filename, flen);
    elf_filename_buf[flen] = '\0';

    /* Create kernel thread that will load ELF and jump to Ring 3 */
    int32_t pid = proc_create("elf_user", elf_thread_fn, 128);
    if (pid < 0) {
        vga_puts_color("  Failed to create process\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }

    /* Yield to let ELF thread run */
    for (int i = 0; i < 30; i++) {
        proc_yield();
    }
    pit_sleep_ms(300);

    vga_puts("  ELF program completed.\n");
    return 0;
}
