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
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/process.h>
#include <kernel/gdt.h>
#include <kernel/usermode.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <lib/string.h>

#define USER_STACK_SIZE 4096

/* User address-space layout (Ring 3): USER_REGION_START (program load base) and
 * USER_STACK_TOP (top of user stack) are defined in <kernel/usermode.h> so the
 * ELF loader and the syscall pointer-validation code share one source of truth.
 * The program loads at USER_REGION_START (see src/user/user.ld); the user stack
 * sits at the top of the low private region, clear of any plausible program. */

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

    /* --- Build a private, isolated address space for this program ----------
     * Kernel memory is supervisor-only (see vmm_init). We create a fresh
     * address space, allocate brand-new physical frames for the program's
     * code and stack, and map them user-accessible (PTE_USER) at their virtual
     * addresses. The kernel stays cloned-in as supervisor for syscall/IRQ
     * handling under this CR3, so Ring 3 can reach ONLY these user frames. */
    uint32_t user_pd = vmm_create_address_space();
    if (!user_pd) {
        vga_puts_color("  ELF: address space alloc failed\n", VGA_LIGHT_RED, VGA_BLACK);
        kfree(file_buf);
        return;
    }

    /* Pass 1: validate PT_LOAD segments and find the page-aligned span. */
    uint32_t segments_loaded = 0;
    uint32_t va_lo = 0xFFFFFFFF, va_hi = 0;
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

        /* Bounds check: keep the program inside the low private region. */
        if (ph->p_vaddr + ph->p_memsz > 0x02000000) {
            vga_puts_color("  ELF: segment out of range!\n", VGA_LIGHT_RED, VGA_BLACK);
            vmm_destroy_address_space(user_pd);
            kfree(file_buf);
            return;
        }
        uint32_t lo = ph->p_vaddr & ~0xFFF;
        uint32_t hi = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFF;
        if (lo < va_lo) va_lo = lo;
        if (hi > va_hi) va_hi = hi;
        segments_loaded++;
    }

    if (segments_loaded == 0) {
        vga_puts_color("  ELF: no loadable segments\n", VGA_LIGHT_RED, VGA_BLACK);
        vmm_destroy_address_space(user_pd);
        kfree(file_buf);
        return;
    }

    /* Map fresh user frames for the code span and one user-stack page. */
    for (uint32_t va = va_lo; va < va_hi; va += PAGE_SIZE) {
        uint32_t frame = pmm_alloc_page();
        if (!frame) {
            vga_puts_color("  ELF: out of physical memory\n", VGA_LIGHT_RED, VGA_BLACK);
            vmm_destroy_address_space(user_pd);
            kfree(file_buf);
            return;
        }
        vmm_map_user_page(user_pd, va, frame);
    }
    uint32_t ustack_page = USER_STACK_TOP - PAGE_SIZE;
    uint32_t sframe = pmm_alloc_page();
    if (!sframe) {
        vga_puts_color("  ELF: out of physical memory\n", VGA_LIGHT_RED, VGA_BLACK);
        vmm_destroy_address_space(user_pd);
        kfree(file_buf);
        return;
    }
    vmm_map_user_page(user_pd, ustack_page, sframe);

    /* Save entry point BEFORE we switch away (hdr points into file_buf). */
    uint32_t entry_point = hdr->e_entry;

    /* Switch to the program's address space. The kernel thread keeps running:
     * all kernel mappings are cloned (supervisor) into user_pd. Recording it
     * in the PCB ensures the scheduler reloads this CR3 if we are preempted
     * while in Ring 3. */
    current_process->page_dir = user_pd;
    vmm_switch_address_space(user_pd);

    /* Pass 2: now that user frames are mapped, copy each segment in and zero
     * its BSS. Ring 0 may write user pages; file_buf is kernel memory, still
     * mapped supervisor under user_pd, so the source read is valid too. */
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        elf32_phdr_t* ph = (elf32_phdr_t*)(file_buf + hdr->e_phoff +
                                             i * hdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        memcpy((void*)ph->p_vaddr, file_buf + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void*)(ph->p_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
    }
    memset((void*)ustack_page, 0, PAGE_SIZE);

    vga_puts_color("  Jumping to Ring 3 (isolated)...\n", VGA_LIGHT_GREEN, VGA_BLACK);

    /* file_buf is kernel memory, still mapped under user_pd — free it. */
    kfree(file_buf);

    uint32_t user_esp = USER_STACK_TOP - 16;

    /* Kernel stack for Ring 3 → Ring 0 transitions (syscalls/IRQs). */
    uint32_t kstack_top = current_process->stack_base + PROC_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);

    /* Jump to Ring 3 at the ELF entry point. The user frames are reclaimed via
     * the address space on exit (vmm_destroy_address_space), so we deliberately
     * do NOT register the stack with proc_set_user_stack (that path kfree()s a
     * heap pointer; our stack is a PMM frame). */
    jump_to_usermode(entry_point, user_esp);
}

/* ---- Ring-3 isolation self-test (shell: sgf) --------------------------------
 * Proves the page-fault policy: a Ring 3 store to a kernel address kills only
 * the faulting process (SEGFAULT) while the shell stays alive. Builds a tiny
 * private address space with the same primitives the ELF loader uses (the
 * loader flow itself is untouched), drops a few bytes of machine code that
 * store to 0x00100000 — a supervisor-only kernel page — and enters Ring 3. */
#define SGF_CODE_VA USER_REGION_START   /* same 4MB user region as user.ld */

static const uint8_t sgf_code[] = {
    0xB8, 0xAD, 0xDE, 0x00, 0x00,   /* mov  $0x0000dead, %eax             */
    0xA3, 0x00, 0x00, 0x10, 0x00,   /* mov  %eax, (0x00100000)  <- kernel */
    0xEB, 0xFE                       /* 1: jmp 1b   (not reached)          */
};

static void sgf_thread_fn(void) {
    uint32_t user_pd = vmm_create_address_space();
    if (!user_pd) {
        vga_puts_color("  SGF: address space alloc failed\n", VGA_LIGHT_RED, VGA_BLACK);
        return;
    }

    uint32_t cframe = pmm_alloc_page();
    uint32_t sframe = pmm_alloc_page();
    if (!cframe || !sframe) {
        vga_puts_color("  SGF: out of physical memory\n", VGA_LIGHT_RED, VGA_BLACK);
        vmm_destroy_address_space(user_pd);
        return;
    }
    vmm_map_user_page(user_pd, SGF_CODE_VA, cframe);
    uint32_t ustack_page = USER_STACK_TOP - PAGE_SIZE;
    vmm_map_user_page(user_pd, ustack_page, sframe);

    current_process->page_dir = user_pd;
    vmm_switch_address_space(user_pd);

    memcpy((void*)SGF_CODE_VA, sgf_code, sizeof(sgf_code));
    memset((void*)ustack_page, 0, PAGE_SIZE);

    vga_puts_color("  Ring 3 will store to kernel 0x00100000 (must SEGFAULT)...\n",
                   VGA_YELLOW, VGA_BLACK);

    uint32_t kstack_top = current_process->stack_base + PROC_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);
    jump_to_usermode(SGF_CODE_VA, USER_STACK_TOP - 16);
}

int32_t elf_run_segfault_test(void) {
    vga_puts_color("=== Ring 3 isolation test (sgf) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    int32_t pid = proc_create("sgf_test", sgf_thread_fn, 128);
    if (pid < 0) {
        vga_puts_color("  Failed to create process\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }
    for (int i = 0; i < 30; i++) proc_yield();
    pit_sleep_ms(300);
    vga_puts("  sgf test complete (shell still alive).\n");
    return 0;
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
