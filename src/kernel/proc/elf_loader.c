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
        /* The image must stay clear of the Ring 3 heap that SYS_SBRK grows
         * from USER_HEAP_START upward, or the two would silently overlap. */
        if (hi > USER_HEAP_START) {
            vga_puts_color("  ELF: segment overlaps the user heap!\n",
                           VGA_LIGHT_RED, VGA_BLACK);
            vmm_destroy_address_space(user_pd);
            kfree(file_buf);
            return;
        }
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

    /* Anchor the Ring 3 heap. SYS_SBRK grows it from here with user-accessible
     * frames mapped into this address space (see proc_sbrk / ABI.md). */
    current_process->mem.user_brk        = USER_HEAP_START;
    current_process->mem.user_brk_mapped = USER_HEAP_START;

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

/* Patch a little-endian 32-bit immediate into hand-assembled Ring 3 code. */
static void patch32(uint8_t* code, uint32_t off, uint32_t val) {
    code[off]     = (uint8_t)(val & 0xFF);
    code[off + 1] = (uint8_t)((val >> 8) & 0xFF);
    code[off + 2] = (uint8_t)((val >> 16) & 0xFF);
    code[off + 3] = (uint8_t)((val >> 24) & 0xFF);
}

/* ---- Ring-3 syscall pointer-guard self-test (shell: sgw) ---------------------
 * Proves syscall pointer validation: a Ring 3 SYS_WRITE with a KERNEL address
 * (0x00100000, len 64) must return -1 and print NOTHING from kernel memory. The
 * program then does a legitimate SYS_WRITE from its own user memory (proving it
 * keeps running with a working ABI) and exits cleanly. Same address-space setup
 * as the sgf test; the loader flow itself is untouched.
 *
 * The program CHECKS the return value: it loads the "rejected" message, and
 * only if EAX != -1 (i.e. the guard failed to reject) swaps in a FAILURE
 * message instead. Either way it does a legitimate SYS_WRITE from its own user
 * memory — proving it kept running with a working ABI — and exits cleanly.
 *
 * Code bytes (patched at runtime with the message addresses/lengths):
 *   mov $1,%eax; mov $0x00100000,%ebx; mov $64,%ecx; int $0x80  ; must be -1
 *   mov $okmsg,%ebx; mov $oklen,%ecx                            ; assume pass
 *   cmp $-1,%eax; je .print                                     ; verdict
 *   mov $failmsg,%ebx; mov $faillen,%ecx                        ; guard broke
 *   .print: mov $1,%eax; int $0x80                              ; legit write
 *   mov $4,%eax; int $0x80                                      ; exit
 *   1: jmp 1b */
static void sgw_thread_fn(void) {
    static uint8_t code[] = {
        0xB8,0x01,0x00,0x00,0x00,   /*  0: mov  $1,%eax                 */
        0xBB,0x00,0x00,0x10,0x00,   /*  5: mov  $0x00100000,%ebx        */
        0xB9,0x40,0x00,0x00,0x00,   /* 10: mov  $64,%ecx                */
        0xCD,0x80,                  /* 15: int  $0x80  (must return -1) */
        0xBB,0x00,0x00,0x00,0x00,   /* 17: mov  $okmsg,%ebx   (patch @18) */
        0xB9,0x00,0x00,0x00,0x00,   /* 22: mov  $oklen,%ecx   (patch @23) */
        0x83,0xF8,0xFF,             /* 27: cmp  $-1,%eax                */
        0x74,0x0A,                  /* 30: je   .print (-> 42)          */
        0xBB,0x00,0x00,0x00,0x00,   /* 32: mov  $failmsg,%ebx (patch @33) */
        0xB9,0x00,0x00,0x00,0x00,   /* 37: mov  $faillen,%ecx (patch @38) */
        0xB8,0x01,0x00,0x00,0x00,   /* 42: .print: mov $1,%eax          */
        0xCD,0x80,                  /* 47: int  $0x80  (legit write)    */
        0xB8,0x04,0x00,0x00,0x00,   /* 49: mov  $4,%eax                 */
        0xCD,0x80,                  /* 54: int  $0x80  (exit)           */
        0xEB,0xFE                   /* 56: 1: jmp 1b                    */
    };
    static const char okmsg[] =
        "  [guard] kernel ptr write rejected (-1); user program still alive\n";
    static const char failmsg[] =
        "  [guard] FAILURE: kernel ptr write did NOT return -1!\n";

    uint32_t user_pd = vmm_create_address_space();
    if (!user_pd) {
        vga_puts_color("  SGW: address space alloc failed\n", VGA_LIGHT_RED, VGA_BLACK);
        return;
    }
    uint32_t cframe = pmm_alloc_page();
    uint32_t sframe = pmm_alloc_page();
    if (!cframe || !sframe) {
        vga_puts_color("  SGW: out of physical memory\n", VGA_LIGHT_RED, VGA_BLACK);
        vmm_destroy_address_space(user_pd);
        return;
    }
    vmm_map_user_page(user_pd, USER_REGION_START, cframe);
    uint32_t ustack_page = USER_STACK_TOP - PAGE_SIZE;
    vmm_map_user_page(user_pd, ustack_page, sframe);

    current_process->page_dir = user_pd;
    vmm_switch_address_space(user_pd);

    /* Place both messages right after the code, and patch their absolute
     * addresses + lengths into the two message-selecting instruction pairs. */
    uint32_t ok_va    = USER_REGION_START + sizeof(code);
    uint32_t ok_len   = sizeof(okmsg) - 1;     /* exclude NUL */
    uint32_t fail_va  = ok_va + ok_len;
    uint32_t fail_len = sizeof(failmsg) - 1;
    patch32(code, 18, ok_va);   patch32(code, 23, ok_len);
    patch32(code, 33, fail_va); patch32(code, 38, fail_len);

    memcpy((void*)USER_REGION_START, code, sizeof(code));
    memcpy((void*)ok_va, okmsg, ok_len);
    memcpy((void*)fail_va, failmsg, fail_len);
    memset((void*)ustack_page, 0, PAGE_SIZE);

    vga_puts_color("  Ring 3 SYS_WRITE(0x00100000, 64) must be rejected...\n",
                   VGA_YELLOW, VGA_BLACK);

    uint32_t kstack_top = current_process->stack_base + PROC_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);
    jump_to_usermode(USER_REGION_START, USER_STACK_TOP - 16);
}

int32_t elf_run_syscall_guard_test(void) {
    vga_puts_color("=== Ring 3 syscall pointer-guard test (sgw) ===\n",
                   VGA_LIGHT_CYAN, VGA_BLACK);
    int32_t pid = proc_create("sgw_test", sgw_thread_fn, 128);
    if (pid < 0) {
        vga_puts_color("  Failed to create process\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }
    for (int i = 0; i < 30; i++) proc_yield();
    pit_sleep_ms(300);
    vga_puts("  sgw test complete (shell still alive).\n");
    return 0;
}

/* ---- Ring-3 sbrk self-test (shell: shb) -------------------------------------
 * Proves SYS_SBRK now hands Ring 3 memory it can actually touch. The program:
 *   1. sbrk(+8192) -> old break in EAX (kept in ESI, preserved across syscalls)
 *   2. writes a dword magic at [brk] and a byte magic at [brk+8191] — the first
 *      and last byte of the allocation — then reads both back and compares
 *   3. SYS_WRITEs a success or FAILURE line accordingly
 *   4. sbrk(+1MB), which must exceed USER_HEAP_MAX -> reports whether it got -1
 *   5. exits cleanly
 *
 * Note the byte-sized magic at +8191: a dword there would straddle into the
 * next (unmapped) page and fault. Same address-space setup as sgf/sgw.
 *
 * Code bytes (message addresses/lengths patched at runtime):
 *   mov $7,%eax; mov $8192,%ebx; int $0x80; mov %eax,%esi
 *   movl $MAGIC32,(%esi); movb $MAGIC8,8191(%esi)
 *   mov (%esi),%edi; movzbl 8191(%esi),%edx
 *   mov $ok1,%ebx; mov $ok1len,%ecx
 *   cmp $MAGIC32,%edi; jne .fail1; cmp $MAGIC8,%edx; je .p1
 *   .fail1: mov $bad1,%ebx; mov $bad1len,%ecx
 *   .p1: mov $1,%eax; int $0x80
 *   mov $7,%eax; mov $0x00100000,%ebx; int $0x80        ; over USER_HEAP_MAX
 *   mov $ok2,%ebx; mov $ok2len,%ecx; cmp $-1,%eax; je .p2
 *   mov $bad2,%ebx; mov $bad2len,%ecx
 *   .p2: mov $1,%eax; int $0x80
 *   mov $4,%eax; int $0x80; 1: jmp 1b */
#define SHB_MAGIC32 0xA1050B0Eu
#define SHB_MAGIC8  0x5Au

static void shb_thread_fn(void) {
    static uint8_t code[] = {
        0xB8,0x07,0x00,0x00,0x00,        /*   0: mov  $7,%eax  (SYS_SBRK)     */
        0xBB,0x00,0x20,0x00,0x00,        /*   5: mov  $8192,%ebx              */
        0xCD,0x80,                       /*  10: int  $0x80  -> old break     */
        0x89,0xC6,                       /*  12: mov  %eax,%esi               */
        0xC7,0x06,0x0E,0x0B,0x05,0xA1,   /*  14: movl $MAGIC32,(%esi)         */
        0xC6,0x86,0xFF,0x1F,0x00,0x00,0x5A, /* 20: movb $MAGIC8,8191(%esi)    */
        0x8B,0x3E,                       /*  27: mov  (%esi),%edi   read back */
        0x0F,0xB6,0x96,0xFF,0x1F,0x00,0x00, /* 29: movzbl 8191(%esi),%edx     */
        0xBB,0x00,0x00,0x00,0x00,        /*  36: mov  $ok1,%ebx    (patch @37) */
        0xB9,0x00,0x00,0x00,0x00,        /*  41: mov  $ok1len,%ecx (patch @42) */
        0x81,0xFF,0x0E,0x0B,0x05,0xA1,   /*  46: cmp  $MAGIC32,%edi           */
        0x75,0x05,                       /*  52: jne  .fail1 (-> 59)          */
        0x83,0xFA,0x5A,                  /*  54: cmp  $MAGIC8,%edx            */
        0x74,0x0A,                       /*  57: je   .p1    (-> 69)          */
        0xBB,0x00,0x00,0x00,0x00,        /*  59: .fail1: mov $bad1,%ebx  (@60) */
        0xB9,0x00,0x00,0x00,0x00,        /*  64: mov  $bad1len,%ecx      (@65) */
        0xB8,0x01,0x00,0x00,0x00,        /*  69: .p1: mov $1,%eax             */
        0xCD,0x80,                       /*  74: int  $0x80  (write verdict)  */
        0xB8,0x07,0x00,0x00,0x00,        /*  76: mov  $7,%eax  (SYS_SBRK)     */
        0xBB,0x00,0x00,0x10,0x00,        /*  81: mov  $0x00100000,%ebx  (1MB) */
        0xCD,0x80,                       /*  86: int  $0x80  (must be -1)     */
        0xBB,0x00,0x00,0x00,0x00,        /*  88: mov  $ok2,%ebx          (@89) */
        0xB9,0x00,0x00,0x00,0x00,        /*  93: mov  $ok2len,%ecx       (@94) */
        0x83,0xF8,0xFF,                  /*  98: cmp  $-1,%eax                */
        0x74,0x0A,                       /* 101: je   .p2   (-> 113)          */
        0xBB,0x00,0x00,0x00,0x00,        /* 103: mov  $bad2,%ebx        (@104) */
        0xB9,0x00,0x00,0x00,0x00,        /* 108: mov  $bad2len,%ecx     (@109) */
        0xB8,0x01,0x00,0x00,0x00,        /* 113: .p2: mov $1,%eax             */
        0xCD,0x80,                       /* 118: int  $0x80  (write verdict)  */
        0xB8,0x04,0x00,0x00,0x00,        /* 120: mov  $4,%eax                 */
        0xCD,0x80,                       /* 125: int  $0x80  (exit)           */
        0xEB,0xFE                        /* 127: 1: jmp 1b                    */
    };
    static const char ok1[]  =
        "  [sbrk] user heap RW ok: magic read back at brk and brk+8191\n";
    static const char bad1[] =
        "  [sbrk] FAILURE: magic read-back mismatch in user heap\n";
    static const char ok2[]  =
        "  [sbrk] over-limit sbrk correctly refused with -1\n";
    static const char bad2[] =
        "  [sbrk] FAILURE: over-limit sbrk did NOT return -1\n";

    uint32_t user_pd = vmm_create_address_space();
    if (!user_pd) {
        vga_puts_color("  SHB: address space alloc failed\n", VGA_LIGHT_RED, VGA_BLACK);
        return;
    }
    uint32_t cframe = pmm_alloc_page();
    uint32_t sframe = pmm_alloc_page();
    if (!cframe || !sframe) {
        vga_puts_color("  SHB: out of physical memory\n", VGA_LIGHT_RED, VGA_BLACK);
        vmm_destroy_address_space(user_pd);
        return;
    }
    vmm_map_user_page(user_pd, USER_REGION_START, cframe);
    uint32_t ustack_page = USER_STACK_TOP - PAGE_SIZE;
    vmm_map_user_page(user_pd, ustack_page, sframe);

    current_process->page_dir = user_pd;
    vmm_switch_address_space(user_pd);

    /* Anchor the Ring 3 heap, exactly as the ELF loader does. */
    current_process->mem.user_brk        = USER_HEAP_START;
    current_process->mem.user_brk_mapped = USER_HEAP_START;

    uint32_t ok1_va   = USER_REGION_START + sizeof(code);
    uint32_t ok1_len  = sizeof(ok1) - 1;
    uint32_t bad1_va  = ok1_va + ok1_len;
    uint32_t bad1_len = sizeof(bad1) - 1;
    uint32_t ok2_va   = bad1_va + bad1_len;
    uint32_t ok2_len  = sizeof(ok2) - 1;
    uint32_t bad2_va  = ok2_va + ok2_len;
    uint32_t bad2_len = sizeof(bad2) - 1;
    patch32(code, 37,  ok1_va);   patch32(code, 42,  ok1_len);
    patch32(code, 60,  bad1_va);  patch32(code, 65,  bad1_len);
    patch32(code, 89,  ok2_va);   patch32(code, 94,  ok2_len);
    patch32(code, 104, bad2_va);  patch32(code, 109, bad2_len);

    memcpy((void*)USER_REGION_START, code, sizeof(code));
    memcpy((void*)ok1_va,  ok1,  ok1_len);
    memcpy((void*)bad1_va, bad1, bad1_len);
    memcpy((void*)ok2_va,  ok2,  ok2_len);
    memcpy((void*)bad2_va, bad2, bad2_len);
    memset((void*)ustack_page, 0, PAGE_SIZE);

    vga_puts_color("  Ring 3 sbrk(+8192), touch both ends, then exceed the limit...\n",
                   VGA_YELLOW, VGA_BLACK);

    uint32_t kstack_top = current_process->stack_base + PROC_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);
    jump_to_usermode(USER_REGION_START, USER_STACK_TOP - 16);
}

int32_t elf_run_sbrk_test(void) {
    vga_puts_color("=== Ring 3 sbrk test (shb) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    uint32_t free_before = g_pmm_stats.free_pages;
    vga_puts("  free_pages before: ");
    vga_put_dec(free_before);
    vga_puts("\n");

    int32_t pid = proc_create("shb_test", shb_thread_fn, 128);
    if (pid < 0) {
        vga_puts_color("  Failed to create process\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }
    for (int i = 0; i < 30; i++) proc_yield();
    pit_sleep_ms(300);

    uint32_t free_after = g_pmm_stats.free_pages;
    vga_puts("  free_pages after:  ");
    vga_put_dec(free_after);
    if (free_after == free_before)
        vga_puts_color("  (no leak)\n", VGA_LIGHT_GREEN, VGA_BLACK);
    else
        vga_puts_color("  (LEAKED!)\n", VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("  shb test complete (shell still alive).\n");
    return 0;
}

/* ---- Ring-3 file-access self-test (shell: shf) -------------------------------
 * Exercises the ABI v1.1 file syscalls from Ring 3. The program:
 *   1. SYS_OPEN("hello.elf") — a file the VFS seeds at boot (vfs_init)
 *   2. SYS_FREAD(fd, buf, 16) with the length in EDX (the third-arg path)
 *   3. hex-encodes those 16 bytes and SYS_WRITEs them. Hex, not raw, because
 *      an ELF header contains NUL bytes and SYS_WRITE stops at the first one —
 *      so this also proves binary reads survive intact.
 *   4. SYS_FCLOSE(fd)
 *   5. SYS_OPEN with a KERNEL address (0x00100000) as the path — user_str_ok
 *      must reject it with a negative return and leak nothing
 *   6. SYS_OPEN once more and deliberately NEVER close it, then exit. That
 *      leaked descriptor is what the fd-cleanup test (requirement 4) watches:
 *      proc_release_fds must hand the global VFS slot back at process exit.
 *
 * The machine code below was produced by GNU as from a real .S source and
 * verified with objdump; absolute addresses are patched in at load time. */
static void shf_thread_fn(void) {
    static uint8_t code[] = {
        0xB8,0x0B,0x00,0x00,0x00,0xBB,0x11,0x11,   /*   0 */
        0x11,0x11,0xCD,0x80,0x89,0xC6,0x83,0xF8,   /*   8 */
        0x00,0x0F,0x8C,0xC8,0x00,0x00,0x00,0xB8,   /*  16 */
        0x0C,0x00,0x00,0x00,0x89,0xF3,0xB9,0x22,   /*  24 */
        0x22,0x22,0x22,0xBA,0x10,0x00,0x00,0x00,   /*  32 */
        0xCD,0x80,0x83,0xF8,0x10,0x0F,0x85,0xBF,   /*  40 */
        0x00,0x00,0x00,0x31,0xC9,0xBB,0x44,0x44,   /*  48 */
        0x44,0x44,0x0F,0xB6,0x81,0x23,0x23,0x23,   /*  56 */
        0x23,0x89,0xC2,0xC1,0xE8,0x04,0x83,0xE2,   /*  64 */
        0x0F,0x0F,0xB6,0x80,0x33,0x33,0x33,0x33,   /*  72 */
        0x0F,0xB6,0x92,0x34,0x34,0x34,0x34,0x88,   /*  80 */
        0x03,0x88,0x53,0x01,0xC6,0x43,0x02,0x20,   /*  88 */
        0x83,0xC3,0x03,0x41,0x83,0xF9,0x10,0x7C,   /*  96 */
        0xD1,0xB8,0x01,0x00,0x00,0x00,0xBB,0x55,   /* 104 */
        0x55,0x55,0x55,0xB9,0x56,0x56,0x56,0x56,   /* 112 */
        0xCD,0x80,0xB8,0x01,0x00,0x00,0x00,0xBB,   /* 120 */
        0x45,0x45,0x45,0x45,0xB9,0x30,0x00,0x00,   /* 128 */
        0x00,0xCD,0x80,0xB8,0x01,0x00,0x00,0x00,   /* 136 */
        0xBB,0x57,0x57,0x57,0x57,0xB9,0x01,0x00,   /* 144 */
        0x00,0x00,0xCD,0x80,0xB8,0x0D,0x00,0x00,   /* 152 */
        0x00,0x89,0xF3,0xCD,0x80,0xB8,0x0B,0x00,   /* 160 */
        0x00,0x00,0xBB,0x00,0x00,0x10,0x00,0xCD,   /* 168 */
        0x80,0xBB,0x66,0x66,0x66,0x66,0xB9,0x67,   /* 176 */
        0x67,0x67,0x67,0x83,0xF8,0x00,0x7C,0x0A,   /* 184 */
        0xBB,0x68,0x68,0x68,0x68,0xB9,0x69,0x69,   /* 192 */
        0x69,0x69,0xB8,0x01,0x00,0x00,0x00,0xCD,   /* 200 */
        0x80,0xB8,0x0B,0x00,0x00,0x00,0xBB,0x12,   /* 208 */
        0x12,0x12,0x12,0xCD,0x80,0xEB,0x2D,0xB8,   /* 216 */
        0x01,0x00,0x00,0x00,0xBB,0x77,0x77,0x77,   /* 224 */
        0x77,0xB9,0x78,0x78,0x78,0x78,0xCD,0x80,   /* 232 */
        0xEB,0x1A,0xB8,0x01,0x00,0x00,0x00,0xBB,   /* 240 */
        0x79,0x79,0x79,0x79,0xB9,0x7A,0x7A,0x7A,   /* 248 */
        0x7A,0xCD,0x80,0xB8,0x0D,0x00,0x00,0x00,   /* 256 */
        0x89,0xF3,0xCD,0x80,0xB8,0x04,0x00,0x00,   /* 264 */
        0x00,0xCD,0x80,0xEB,0xFE,   /* 272 */
    };
    /* Patch offsets, located automatically from the assembled placeholders. */
    enum { P_PATH=6, P_PATH2=215, P_BUF=31, P_BUF_IDX=61, P_HEX_HI=76, P_HEX_LO=83,
           P_OUT_CUR=54, P_OUT_W=128, P_PREFIX=111, P_PREFIX_LEN=116, P_NL=145,
           P_OK2=178, P_OK2_LEN=183, P_BAD2=193, P_BAD2_LEN=198,
           P_BADOPEN=229, P_BADOPEN_LEN=234, P_BADREAD=248, P_BADREAD_LEN=253 };

    static const char path[]   = "hello.elf";
    static const char hextab[] = "0123456789ABCDEF";
    static const char prefix[] = "  [file] hello.elf[0..15] = ";
    static const char nl[]     = "\n";
    static const char ok2[]    =
        "  [file] kernel-pointer path rejected (negative); nothing leaked\n";
    static const char bad2[]   =
        "  [file] FAILURE: kernel-pointer path was NOT rejected!\n";
    static const char bad_open[] =
        "  [file] FAILURE: could not open hello.elf\n";
    static const char bad_read[] =
        "  [file] FAILURE: SYS_FREAD did not return 16 bytes\n";

    uint32_t user_pd = vmm_create_address_space();
    if (!user_pd) {
        vga_puts_color("  SHF: address space alloc failed\n", VGA_LIGHT_RED, VGA_BLACK);
        return;
    }
    uint32_t cframe = pmm_alloc_page();
    uint32_t sframe = pmm_alloc_page();
    if (!cframe || !sframe) {
        vga_puts_color("  SHF: out of physical memory\n", VGA_LIGHT_RED, VGA_BLACK);
        vmm_destroy_address_space(user_pd);
        return;
    }
    vmm_map_user_page(user_pd, USER_REGION_START, cframe);
    uint32_t ustack_page = USER_STACK_TOP - PAGE_SIZE;
    vmm_map_user_page(user_pd, ustack_page, sframe);

    current_process->page_dir = user_pd;
    vmm_switch_address_space(user_pd);

    /* Lay code, then read-only data, then the two scratch buffers, into the
     * single user code page. Everything stays well inside 4 KB. */
    uint32_t cur        = USER_REGION_START + sizeof(code);
    uint32_t path_va    = cur;                cur += sizeof(path);
    uint32_t hextab_va  = cur;                cur += sizeof(hextab) - 1;
    uint32_t prefix_va  = cur;                cur += sizeof(prefix) - 1;
    uint32_t nl_va      = cur;                cur += 1;
    uint32_t ok2_va     = cur;                cur += sizeof(ok2) - 1;
    uint32_t bad2_va    = cur;                cur += sizeof(bad2) - 1;
    uint32_t badopen_va = cur;                cur += sizeof(bad_open) - 1;
    uint32_t badread_va = cur;                cur += sizeof(bad_read) - 1;
    uint32_t buf_va     = cur;                cur += 16;   /* SYS_FREAD target */
    uint32_t out_va     = cur;                cur += 48;   /* "XX " * 16       */

    patch32(code, P_PATH,        path_va);
    patch32(code, P_PATH2,       path_va);
    patch32(code, P_BUF,         buf_va);
    patch32(code, P_BUF_IDX,     buf_va);
    patch32(code, P_HEX_HI,      hextab_va);
    patch32(code, P_HEX_LO,      hextab_va);
    patch32(code, P_OUT_CUR,     out_va);
    patch32(code, P_OUT_W,       out_va);
    patch32(code, P_PREFIX,      prefix_va);
    patch32(code, P_PREFIX_LEN,  sizeof(prefix) - 1);
    patch32(code, P_NL,          nl_va);
    patch32(code, P_OK2,         ok2_va);
    patch32(code, P_OK2_LEN,     sizeof(ok2) - 1);
    patch32(code, P_BAD2,        bad2_va);
    patch32(code, P_BAD2_LEN,    sizeof(bad2) - 1);
    patch32(code, P_BADOPEN,     badopen_va);
    patch32(code, P_BADOPEN_LEN, sizeof(bad_open) - 1);
    patch32(code, P_BADREAD,     badread_va);
    patch32(code, P_BADREAD_LEN, sizeof(bad_read) - 1);

    memcpy((void*)USER_REGION_START, code, sizeof(code));
    memcpy((void*)path_va,    path,     sizeof(path));        /* with NUL */
    memcpy((void*)hextab_va,  hextab,   sizeof(hextab) - 1);
    memcpy((void*)prefix_va,  prefix,   sizeof(prefix) - 1);
    memcpy((void*)nl_va,      nl,       1);
    memcpy((void*)ok2_va,     ok2,      sizeof(ok2) - 1);
    memcpy((void*)bad2_va,    bad2,     sizeof(bad2) - 1);
    memcpy((void*)badopen_va, bad_open, sizeof(bad_open) - 1);
    memcpy((void*)badread_va, bad_read, sizeof(bad_read) - 1);
    memset((void*)buf_va, 0, 16 + 48);
    memset((void*)ustack_page, 0, PAGE_SIZE);

    vga_puts_color("  Ring 3 open/fread/close on hello.elf, then a kernel-ptr path...\n",
                   VGA_YELLOW, VGA_BLACK);

    uint32_t kstack_top = current_process->stack_base + PROC_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);
    jump_to_usermode(USER_REGION_START, USER_STACK_TOP - 16);
}

int32_t elf_run_file_test(void) {
    vga_puts_color("=== Ring 3 file syscall test (shf) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    uint32_t fds_before = g_vfs_stats.open_fds;
    vga_puts("  VFS open_fds before: ");
    vga_put_dec(fds_before);
    vga_puts("\n");

    int32_t pid = proc_create("shf_test", shf_thread_fn, 128);
    if (pid < 0) {
        vga_puts_color("  Failed to create process\n", VGA_LIGHT_RED, VGA_BLACK);
        return -1;
    }
    for (int i = 0; i < 30; i++) proc_yield();
    pit_sleep_ms(300);

    uint32_t fds_after = g_vfs_stats.open_fds;
    vga_puts("  VFS open_fds after:  ");
    vga_put_dec(fds_after);
    if (fds_after == fds_before)
        vga_puts_color("  (leaked fd reclaimed)\n", VGA_LIGHT_GREEN, VGA_BLACK);
    else
        vga_puts_color("  (LEAKED!)\n", VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("  shf test complete (shell still alive).\n");
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
