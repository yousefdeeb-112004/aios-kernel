/* =============================================================================
 * Process Management + Signals + sbrk + Address Spaces + SMP Scheduling
 *
 * Features:
 *   - Per-process page directory (CR3 switched on context switch)
 *   - SMP-safe scheduling with spinlock on process table
 *   - Processes can run on any online CPU core
 *   - POSIX-style signals
 *   - Per-process heap via sbrk()
 * ============================================================================= */

#include <kernel/process.h>
#include <kernel/heap.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/log.h>
#include <kernel/lock.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/usermode.h>
#include <kernel/vfs.h>
#include <kernel/smp.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <lib/string.h>

static process_t pt[MAX_PROCESSES];
process_t* current_process = NULL;
static uint32_t next_pid = 0;
scheduler_stats_t g_sched_stats;
static volatile bool in_schedule = false;

/* SMP: per-CPU current process tracking */
static process_t* cpu_current[MAX_CPUS];
static spinlock_t sched_lock = SPINLOCK_INIT;

static void schedule(void);

static void reap_terminated(void);

static void recycle_terminated(void) {
    /* Release resources before the slot loses its TERMINATED marker, or the
     * reaper would never see it again. */
    reap_terminated();
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (pt[i].state == PROC_TERMINATED)
            pt[i].state = PROC_UNUSED;
    }
}

/* Free the kernel stack, user stack, kmalloc heap and private address space of
 * every process that has already exited and switched away.
 *
 * This must NOT be done from schedule()'s own frame: switch_context() swaps the
 * kernel stack mid-function, so anything read after that call comes from the
 * RESUMING process's frame, not ours. Scanning the process table instead is
 * idempotent and stateless — it cannot lose a record no matter which process
 * happens to run it, and re-running it is harmless because every field is
 * cleared as it is released.
 *
 * Safe points: after the context switch (CR3 already points at the new process,
 * so a dying address space is no longer live) and never for current_process. */
/* Close VFS descriptors the process still had open. The per-process table is
 * reset when its slot is reused, but the VFS fd it pointed at is a GLOBAL slot
 * (VFS_MAX_OPEN = 32 system-wide), so a user program that exits without
 * SYS_FCLOSE would pin one forever. Idempotent: cleared slots are skipped. */
static void proc_release_fds(process_t* p) {
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (p->fds[i].used && p->fds[i].type == FD_FILE && p->fds[i].target >= 0)
            vfs_close(p->fds[i].target);
        p->fds[i].used = false;
        p->fds[i].type = FD_NONE;
        p->fds[i].target = -1;
    }
}

static void reap_terminated(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) {
        process_t* p = &pt[i];
        if (p->state != PROC_TERMINATED || p == current_process) continue;
        proc_release_fds(p);
        if (p->stack_base)     { kfree((void*)p->stack_base);     p->stack_base = 0; }
        if (p->user_stack)     { kfree((void*)p->user_stack);     p->user_stack = 0; }
        if (p->mem.heap_start) { kfree((void*)p->mem.heap_start); p->mem.heap_start = 0; }
        if (p->page_dir && p->page_dir != vmm_get_kernel_page_dir()) {
            /* Frees every PTE_USER frame in the private page table — program
             * image, user stack AND sbrk heap pages — and returns the pool
             * slot. See vmm_destroy_address_space(). */
            vmm_destroy_address_space(p->page_dir);
            p->page_dir = 0;
        }
    }
}

void proc_init(void) {
    memset(pt, 0, sizeof(pt));
    memset(cpu_current, 0, sizeof(cpu_current));
    memset(&g_sched_stats, 0, sizeof(scheduler_stats_t));
    next_pid = 0;

    process_t* p = &pt[0];
    p->pid = next_pid++;
    p->state = PROC_RUNNING;
    strcpy(p->name, "kernel");
    p->priority = 128;
    p->exit_code = 0;
    p->page_dir = vmm_get_kernel_page_dir();
    p->cpu_id = 0;

    current_process = p;
    cpu_current[0] = p;
    g_sched_stats.total_created = 1;
    g_sched_stats.active_count = 1;
}

int32_t proc_create(const char* name, void (*entry)(void), uint32_t prio) {
    spin_lock(&sched_lock);

    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (pt[i].state == PROC_UNUSED) { slot = i; break; }

    if (slot < 0) {
        recycle_terminated();
        for (int i = 0; i < MAX_PROCESSES; i++)
            if (pt[i].state == PROC_UNUSED) { slot = i; break; }
    }
    if (slot < 0) { spin_unlock(&sched_lock); return -1; }

    void* stk = kmalloc(PROC_STACK_SIZE);
    if (!stk) { spin_unlock(&sched_lock); return -1; }
    memset(stk, 0, PROC_STACK_SIZE);

    process_t* p = &pt[slot];
    memset(p, 0, sizeof(process_t));
    p->pid = next_pid++;
    p->state = PROC_READY;
    strcpy(p->name, name);
    p->priority = prio;
    p->stack_base = (uint32_t)stk;
    p->ai.created_at_tick = pit_get_ticks();
    p->exit_code = 0;
    p->waited = false;
    p->parent_pid = current_process ? current_process->pid : 0;
    p->cpu_id = 0xFF;

    /* Kernel threads share the kernel page directory.
     * Only user-mode ELF programs get their own address space
     * (created by the ELF loader via vmm_create_address_space). */
    p->page_dir = vmm_get_kernel_page_dir();

    /* Initialize signal handlers to default */
    for (int i = 0; i < MAX_SIGNALS; i++)
        p->sig.handlers[i] = SIG_DFL;

    /* Initialize fd table: stdin/stdout/stderr → console */
    proc_init_fds(p);

    /* Allocate per-process heap (4KB initial) */
    void* heap = kmalloc(4096);
    if (heap) {
        p->mem.heap_start = (uint32_t)heap;
        p->mem.heap_break = (uint32_t)heap;
        p->mem.heap_max = (uint32_t)heap + 4096;
    }

    uint32_t top = (uint32_t)stk + PROC_STACK_SIZE;
    uint32_t* sp = (uint32_t*)top;
    *(--sp) = (uint32_t)proc_exit;
    *(--sp) = (uint32_t)entry;
    *(--sp) = 0; /* ebp */
    *(--sp) = 0; /* ebx */
    *(--sp) = 0; /* esi */
    *(--sp) = 0; /* edi */
    p->esp = (uint32_t)sp;

    g_sched_stats.total_created++;
    g_sched_stats.active_count++;
    spin_unlock(&sched_lock);
    return (int32_t)p->pid;
}

void proc_exit(void) {
    if (!current_process || current_process->pid == 0) return;
    __asm__ volatile("cli");
    current_process->state = PROC_TERMINATED;
    g_sched_stats.total_terminated++;
    g_sched_stats.active_count--;
    schedule();
    for (;;) __asm__ volatile("hlt");
}

static void schedule(void) {
    if (in_schedule) return;
    in_schedule = true;

    int cur = -1;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (&pt[i] == current_process) { cur = i; break; }
    if (cur < 0) { in_schedule = false; __asm__ volatile("sti"); return; }

    /* Find next READY process */
    int nx = -1;
    for (int i = 1; i <= MAX_PROCESSES; i++) {
        int idx = (cur + i) % MAX_PROCESSES;
        if (pt[idx].state == PROC_READY) { nx = idx; break; }
    }

    if (nx < 0 || &pt[nx] == current_process) {
        in_schedule = false;
        __asm__ volatile("sti");
        return;
    }

    process_t* old = current_process;
    process_t* nw = &pt[nx];

    if (old->state == PROC_RUNNING) old->state = PROC_READY;
    old->cpu_id = 0xFF;

    nw->state = PROC_RUNNING;
    nw->ai.context_switches++;
    nw->cpu_id = 0; /* BSP for now; APs set their own */
    g_sched_stats.total_switches++;
    current_process = nw;

    /* Switch address space if different */
    if (nw->page_dir && nw->page_dir != old->page_dir) {
        vmm_switch_address_space(nw->page_dir);
    }

    in_schedule = false;
    switch_context(&old->esp, nw->esp);
    __asm__ volatile("sti");

    /* We are now on the resuming process's stack: `old` and any local we set
     * before the switch are stale. Reap from the process table instead. */
    reap_terminated();

    proc_check_signals();
}

void proc_yield(void) {
    schedule();
}

static bool preempt_on = false;
static uint32_t time_slice = 5;

static void sched_timer(registers_t* r) {
    (void)r;
    g_timer_stats.ticks++;
    g_timer_stats.irq_count++;
    if (g_timer_stats.ticks % PIT_TARGET == 0)
        g_timer_stats.seconds++;
    log_set_tick(g_timer_stats.ticks);
    if (current_process) current_process->ai.ticks_used++;
    pic_send_eoi(0);

    if (preempt_on && g_sched_stats.active_count > 1 &&
        (g_timer_stats.ticks % time_slice == 0) && !in_schedule) {
        schedule();
    }
}

void proc_enable_preemption(void) {
    preempt_on = true;
    idt_register_handler(32, sched_timer);
}

/* ========================================================================= */
/* Signal System                                                              */
/* ========================================================================= */

const char* signal_name(int signum) {
    switch (signum) {
        case SIGHUP:  return "SIGHUP";  case SIGINT:  return "SIGINT";
        case SIGKILL: return "SIGKILL"; case SIGUSR1: return "SIGUSR1";
        case SIGUSR2: return "SIGUSR2"; case SIGTERM: return "SIGTERM";
        case SIGSTOP: return "SIGSTOP"; case SIGCONT: return "SIGCONT";
        case SIGCHLD: return "SIGCHLD"; default:      return "SIG???";
    }
}

int32_t proc_signal(uint32_t pid, int signum) {
    if (signum < 0 || signum >= MAX_SIGNALS) return -1;
    process_t* p = proc_find(pid);
    if (!p) return -2;
    if (pid == 0 && signum != SIGCHLD) return -3;
    g_sched_stats.total_signals++;

    if (signum == SIGCONT) {
        if (p->state == PROC_STOPPED) {
            p->state = PROC_READY;
            g_sched_stats.active_count++;
            if (g_sched_stats.total_stopped > 0) g_sched_stats.total_stopped--;
            p->sig.delivered++;
        }
        p->sig.pending &= ~(1 << SIGSTOP);
        return 0;
    }
    if (signum == SIGKILL) {
        p->sig.delivered++;
        if (p->state == PROC_STOPPED && g_sched_stats.total_stopped > 0)
            g_sched_stats.total_stopped--;
        return proc_kill(pid);
    }
    if (signum == SIGSTOP) {
        if (p->state == PROC_READY || p->state == PROC_RUNNING) {
            if (p == current_process) {
                p->state = PROC_STOPPED;
                g_sched_stats.active_count--;
                g_sched_stats.total_stopped++;
                p->sig.delivered++;
                schedule();
                return 0;
            }
            p->state = PROC_STOPPED;
            g_sched_stats.active_count--;
            g_sched_stats.total_stopped++;
            p->sig.delivered++;
        }
        return 0;
    }
    p->sig.pending |= (1 << signum);
    return 0;
}

int32_t proc_sigaction(int signum, signal_handler_t handler) {
    if (!current_process) return -1;
    if (signum < 0 || signum >= MAX_SIGNALS) return -1;
    if (signum == SIGKILL || signum == SIGSTOP) return -2;
    current_process->sig.handlers[signum] = handler;
    return 0;
}

void proc_check_signals(void) {
    if (!current_process || current_process->pid == 0) return;
    if (current_process->sig.pending == 0) return;

    for (int sig = 1; sig < MAX_SIGNALS; sig++) {
        if (!(current_process->sig.pending & (1 << sig))) continue;
        current_process->sig.pending &= ~(1 << sig);
        current_process->sig.delivered++;
        signal_handler_t handler = current_process->sig.handlers[sig];

        if (handler == SIG_IGN) { current_process->sig.ignored++; continue; }
        if (handler == SIG_DFL) {
            if (sig == SIGCHLD) { current_process->sig.ignored++; continue; }
            current_process->exit_code = sig;
            proc_exit();
            return;
        }
        current_process->sig.caught++;
        handler(sig);
    }
}

/* ========================================================================= */
/* sbrk                                                                       */
/* ========================================================================= */

/* vmm_map_user_page backs exactly ONE page-directory slot per address space
 * (see the KNOWN TRAP comment in vmm.c), so the whole user heap must share the
 * program's 4MB slot. Verified here rather than assumed. */
_Static_assert((USER_HEAP_START >> 22) == (USER_REGION_START >> 22) &&
               ((USER_HEAP_MAX - 1u) >> 22) == (USER_REGION_START >> 22),
               "user heap must live in the same 4MB PDE slot as the program");
_Static_assert(USER_HEAP_START >= USER_REGION_START &&
               USER_HEAP_MAX > USER_HEAP_START &&
               USER_HEAP_MAX <= USER_STACK_TOP - 2u * PAGE_SIZE,
               "user heap must leave a guard page below the user stack page");

/* Ring 3 sbrk: the break is backed by fresh PMM frames mapped PTE_USER into
 * the calling process's own page directory, so the returned pointer is really
 * dereferenceable from user mode.
 *
 * Failure policy: NO ROLLBACK. If PMM runs dry part-way through a grow, the
 * pages already mapped stay mapped and stay recorded in user_brk_mapped; the
 * break is left untouched and -1 is returned. Nothing leaks — those frames are
 * PTE_USER entries in the private page table, so vmm_destroy_address_space
 * reclaims them at process exit, and a later successful sbrk reuses them
 * instead of double-allocating the same virtual page.
 *
 * Shrink (increment < 0) moves the break down but does NOT unmap or free the
 * pages above it (v1 simplification, documented in ABI.md). */
static void* user_sbrk(int32_t increment) {
    process_t*   p   = current_process;
    proc_mem_t*  mem = &p->mem;

    /* First touch: anchor the heap. The ELF loader also does this explicitly;
     * this is the safety net for any other adopter of a user address space. */
    if (mem->user_brk < USER_HEAP_START) {
        mem->user_brk        = USER_HEAP_START;
        mem->user_brk_mapped = USER_HEAP_START;
    }

    uint32_t old_break = mem->user_brk;
    if (increment == 0) return (void*)old_break;

    if (increment < 0) {
        /* 0u - (uint32_t)increment is |increment| without signed-overflow UB. */
        uint32_t dec = 0u - (uint32_t)increment;
        uint32_t room = old_break - USER_HEAP_START;
        mem->user_brk = (dec >= room) ? USER_HEAP_START : (old_break - dec);
        mem->sbrk_calls++;
        return (void*)old_break;
    }

    uint32_t inc       = (uint32_t)increment;
    uint32_t new_break = old_break + inc;
    if (new_break < old_break) return (void*)-1;        /* wrapped */
    if (new_break > USER_HEAP_MAX) return (void*)-1;    /* would hit guard page */

    /* Map every page the break newly crosses. user_brk_mapped is the exclusive
     * end of what is already backed, so this is idempotent across retries. */
    uint32_t need_top = (new_break + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    while (mem->user_brk_mapped < need_top) {
        uint32_t frame = pmm_alloc_page();
        if (!frame) return (void*)-1;                   /* PMM exhausted */
        uint32_t va = mem->user_brk_mapped;
        vmm_map_user_page(p->page_dir, va, frame);
        /* Zero through the new mapping (CR3 is this process's page directory),
         * so a recycled frame never hands stale data to Ring 3. */
        memset((void*)va, 0, PAGE_SIZE);
        mem->user_brk_mapped = va + PAGE_SIZE;
    }

    mem->user_brk = new_break;
    mem->total_allocated += inc;
    mem->sbrk_calls++;
    return (void*)old_break;
}

void* proc_sbrk(int32_t increment) {
    if (!current_process) return (void*)-1;
    proc_mem_t* mem = &current_process->mem;

    /* A process with a private address space is (or is about to be) in Ring 3:
     * give it a real user-accessible heap. Kernel threads share the kernel page
     * directory and keep the kmalloc-backed path below, unchanged. */
    if (current_process->page_dir &&
        current_process->page_dir != vmm_get_kernel_page_dir())
        return user_sbrk(increment);

    if (mem->heap_start == 0) {
        void* heap = kmalloc(16384);
        if (!heap) return (void*)-1;
        mem->heap_start = (uint32_t)heap;
        mem->heap_break = (uint32_t)heap;
        mem->heap_max = (uint32_t)heap + 16384;
    }

    uint32_t old_break = mem->heap_break;
    if (increment == 0) return (void*)old_break;

    uint32_t new_break = old_break + (uint32_t)increment;
    if (increment > 0 && new_break > mem->heap_max) {
        uint32_t need = new_break - mem->heap_max;
        uint32_t extend = (need + 4095) & ~4095;
        void* extra = kmalloc(extend);
        if (!extra) return (void*)-1;
        if ((uint32_t)extra == mem->heap_max) {
            mem->heap_max += extend;
        } else {
            uint32_t total_size = (new_break - mem->heap_start) + 4096;
            void* new_heap = kmalloc(total_size);
            if (!new_heap) { kfree(extra); return (void*)-1; }
            memcpy(new_heap, (void*)mem->heap_start, old_break - mem->heap_start);
            kfree((void*)mem->heap_start);
            kfree(extra);
            uint32_t offset = old_break - mem->heap_start;
            mem->heap_start = (uint32_t)new_heap;
            old_break = mem->heap_start + offset;
            mem->heap_break = old_break;
            new_break = old_break + (uint32_t)increment;
            mem->heap_max = mem->heap_start + total_size;
        }
    } else if (increment < 0 && new_break < mem->heap_start) {
        new_break = mem->heap_start;
    }

    mem->heap_break = new_break;
    mem->total_allocated += (increment > 0) ? (uint32_t)increment : 0;
    mem->sbrk_calls++;
    return (void*)old_break;
}

/* ========================================================================= */
/* Kill + Find + Dump                                                         */
/* ========================================================================= */

int32_t proc_kill(uint32_t pid) {
    if (pid == 0) return -1;
    uint32_t flags;
    irq_save(&flags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pt[i].state == PROC_UNUSED || pt[i].pid != pid) continue;

        if (&pt[i] == current_process && pt[i].state == PROC_RUNNING) {
            irq_restore(flags);
            proc_exit();
            return 0;
        }

        if (pt[i].state == PROC_READY || pt[i].state == PROC_RUNNING) {
            pt[i].state = PROC_TERMINATED;
            g_sched_stats.total_terminated++;
            g_sched_stats.total_killed++;
            g_sched_stats.active_count--;
        } else if (pt[i].state == PROC_STOPPED) {
            pt[i].state = PROC_TERMINATED;
            g_sched_stats.total_terminated++;
            g_sched_stats.total_killed++;
            if (g_sched_stats.total_stopped > 0) g_sched_stats.total_stopped--;
        }

        proc_release_fds(&pt[i]);
        if (pt[i].stack_base) { kfree((void*)pt[i].stack_base); pt[i].stack_base = 0; }
        if (pt[i].user_stack) { kfree((void*)pt[i].user_stack); pt[i].user_stack = 0; }
        if (pt[i].mem.heap_start) { kfree((void*)pt[i].mem.heap_start); pt[i].mem.heap_start = 0; }
        if (pt[i].page_dir && pt[i].page_dir != vmm_get_kernel_page_dir()) {
            vmm_destroy_address_space(pt[i].page_dir);
            pt[i].page_dir = 0;
        }
        pt[i].state = PROC_UNUSED;
        irq_restore(flags);
        return 0;
    }
    irq_restore(flags);
    return -2;
}

process_t* proc_find(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (pt[i].state != PROC_UNUSED && pt[i].pid == pid) return &pt[i];
    return NULL;
}

void proc_set_user_stack(uint32_t ustack) {
    if (current_process) current_process->user_stack = ustack;
}

void proc_dump(void) {
    vga_puts_color("=== Processes ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pt[i].state == PROC_UNUSED) continue;
        process_t* p = &pt[i];
        vga_puts("  PID ");
        vga_put_dec(p->pid);
        vga_puts(": ");
        vga_puts(p->name);
        for (uint32_t j = strlen(p->name); j < 14; j++) vga_putchar(' ');
        switch (p->state) {
            case PROC_READY:      vga_puts_color("READY ", VGA_YELLOW, VGA_BLACK); break;
            case PROC_RUNNING:    vga_puts_color("RUN   ", VGA_LIGHT_GREEN, VGA_BLACK); break;
            case PROC_TERMINATED: vga_puts_color("TERM  ", VGA_DARK_GREY, VGA_BLACK); break;
            case PROC_STOPPED:    vga_puts_color("STOP  ", VGA_LIGHT_RED, VGA_BLACK); break;
            default: vga_puts("???   ");
        }
        vga_puts(" sw:"); vga_put_dec(p->ai.context_switches);
        vga_puts(" t:"); vga_put_dec(p->ai.ticks_used);
        if (p->cpu_id != 0xFF) {
            vga_puts(" cpu:"); vga_put_dec(p->cpu_id);
        }
        if (p->sig.delivered > 0) {
            vga_puts(" sig:"); vga_put_dec(p->sig.delivered);
        }
        if (p->mem.heap_start && p->mem.heap_break > p->mem.heap_start) {
            vga_puts(" mem:"); vga_put_dec(p->mem.heap_break - p->mem.heap_start); vga_putchar('B');
        }
        if (p->page_dir && p->page_dir != vmm_get_kernel_page_dir()) {
            vga_puts_color(" [own-AS]", VGA_LIGHT_CYAN, VGA_BLACK);
        }
        vga_puts("\n");
    }
    vga_puts("  Ctx: "); vga_put_dec(g_sched_stats.total_switches);
    vga_puts("  Signals: "); vga_put_dec(g_sched_stats.total_signals);
    vga_puts("  AddrSpaces: "); vga_put_dec(g_vmm_stats.addr_spaces_created);
    vga_puts("  CR3sw: "); vga_put_dec(g_vmm_stats.cr3_switches);
    if (g_sched_stats.total_stopped > 0) {
        vga_puts("  Stopped: "); vga_put_dec(g_sched_stats.total_stopped);
    }
    vga_puts("\n");
}

/* =========================================================================
 * Per-process file descriptor table
 * ========================================================================= */
void proc_init_fds(process_t* p) {
    memset(p->fds, 0, sizeof(p->fds));
    /* fd 0 = stdin (console) */
    p->fds[FD_STDIN].type = FD_CONSOLE;
    p->fds[FD_STDIN].target = -1;
    p->fds[FD_STDIN].used = true;
    /* fd 1 = stdout (console) */
    p->fds[FD_STDOUT].type = FD_CONSOLE;
    p->fds[FD_STDOUT].target = -1;
    p->fds[FD_STDOUT].used = true;
    /* fd 2 = stderr (console) */
    p->fds[FD_STDERR].type = FD_CONSOLE;
    p->fds[FD_STDERR].target = -1;
    p->fds[FD_STDERR].used = true;
}

int32_t proc_alloc_fd(fd_type_t type, int32_t target) {
    if (!current_process) return -1;
    for (int i = 3; i < PROC_MAX_FDS; i++) {
        if (!current_process->fds[i].used) {
            current_process->fds[i].type = type;
            current_process->fds[i].target = target;
            current_process->fds[i].used = true;
            return i;
        }
    }
    return -1; /* No free fds */
}

void proc_close_fd(int32_t fd) {
    if (!current_process) return;
    if (fd < 0 || fd >= PROC_MAX_FDS) return;
    current_process->fds[fd].type = FD_NONE;
    current_process->fds[fd].target = -1;
    current_process->fds[fd].used = false;
}

/* Resolve a caller-supplied fd to its backing target, or -1.
 *
 * Ownership is structural: the table lives in the caller's own PCB, so a
 * process can only ever name its own descriptors — there is no global fd space
 * for it to reach into. On top of that this checks the index is in range, the
 * slot is actually open, and it is of the expected kind (so Ring 3 cannot pass
 * fd 0/1/2 — the FD_CONSOLE stdio slots — to a file syscall). */
int32_t proc_fd_target(int32_t fd, fd_type_t type) {
    if (!current_process) return -1;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -1;
    proc_fd_t* e = &current_process->fds[fd];
    if (!e->used || e->type != type || e->target < 0) return -1;
    return e->target;
}

/* =========================================================================
 * waitpid — wait for child process to finish
 * ========================================================================= */
int32_t proc_waitpid(uint32_t pid, int32_t* status) {
    if (!current_process) return -1;

    process_t* child = proc_find(pid);
    if (!child) return -2; /* Not found */
    if (child->parent_pid != current_process->pid) return -3; /* Not our child */

    /* Spin-wait for child to terminate */
    uint32_t timeout = 0;
    while (child->state != PROC_TERMINATED && child->state != PROC_UNUSED) {
        proc_yield();
        timeout++;
        if (timeout > 100000) return -4; /* Timeout */
        child = proc_find(pid);
        if (!child) break;
    }

    if (child && status) *status = child->exit_code;
    if (child) child->waited = true;

    /* Send SIGCHLD to parent */
    return 0;
}
