#include <kernel/syscall.h>
#include <kernel/idt.h>
#include <kernel/process.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/usermode.h>
#include <kernel/vfs.h>
#include <drivers/vga.h>
#include <drivers/keyboard.h>
#include <drivers/pit.h>
#include <lib/string.h>

syscall_stats_t g_syscall_stats;

/* True iff the byte range [ptr, ptr+len) lies entirely inside the Ring 3 user
 * region [USER_REGION_START, USER_STACK_TOP). Overflow-safe. An empty range
 * (len == 0) dereferences nothing and is always accepted. Used to vet raw
 * pointers handed to the kernel by user programs before they are dereferenced
 * in Ring 0. */
static bool user_range_ok(uint32_t ptr, uint32_t len) {
    if (len == 0) return true;
    if (ptr + len < ptr) return false;              /* integer overflow */
    if (ptr < USER_REGION_START) return false;
    if (ptr + len > USER_STACK_TOP) return false;
    return true;
}

/* True iff `ptr` points at a NUL-terminated string lying entirely inside the
 * Ring 3 user region. Strings are harder than buffers: the length is not given,
 * so the scan itself could run off the end of mapped memory. We therefore clamp
 * the scan to min(maxlen, bytes remaining before USER_STACK_TOP) — the loop can
 * never touch a byte at or above USER_STACK_TOP, and a string with no
 * terminator inside that window is rejected rather than truncated.
 *
 * Callers must still COPY the string into kernel memory before using it: this
 * only proves a terminator existed at inspection time, and user memory can be
 * rewritten afterwards (TOCTOU). */
static bool user_str_ok(uint32_t ptr, uint32_t maxlen) {
    if (ptr < USER_REGION_START || ptr >= USER_STACK_TOP) return false;
    uint32_t avail = USER_STACK_TOP - ptr;
    if (maxlen > avail) maxlen = avail;
    const char* s = (const char*)ptr;
    for (uint32_t i = 0; i < maxlen; i++)
        if (s[i] == '\0') return true;
    return false;                                   /* no terminator in range */
}

/* Snapshot a path into a kernel buffer of VFS_MAX_NAME bytes, always
 * NUL-terminated. The caller has already range-checked `s` for Ring 3. */
static void copy_path(char* dst, const char* s) {
    uint32_t i = 0;
    while (i < VFS_MAX_NAME - 1 && s[i]) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
}

static void sh(registers_t* r) {
    uint32_t n = r->eax;
    uint32_t a1 = r->ebx;
    uint32_t a2 = r->ecx;
    /* Third argument. ABI v1 reserved EDX for exactly this; the ISR stub's
     * `pushal` saves it and `popal` restores it, and registers_t lays it out
     * in the matching slot, so reading it here is safe for every caller. */
    uint32_t a3 = r->edx;

    /* Pointer arguments are validated only for Ring 3 callers (saved CS RPL 3).
     * Kernel threads reach these syscalls via the int 0x80 wrappers below and
     * pass trusted kernel pointers, so they keep the original behavior. */
    bool from_user = (r->cs & 3) == 3;

    g_syscall_stats.total++;
    if (n < SYS_MAX) g_syscall_stats.counts[n]++;

    switch (n) {
        case SYS_WRITE: {
            if (from_user && !user_range_ok(a1, a2)) { r->eax = (uint32_t)-1; break; }
            const char* b = (const char*)a1;
            for (uint32_t i = 0; i < a2 && b[i]; i++)
                vga_putchar(b[i]);
            r->eax = a2;
            break;
        }
        case SYS_READ: {
            if (from_user && !user_range_ok(a1, a2)) { r->eax = (uint32_t)-1; break; }
            char* b = (char*)a1;
            uint32_t rd = 0;
            for (uint32_t i = 0; i < a2; i++) {
                char c = keyboard_getchar();
                if (!c) break;
                b[i] = c; rd++;
            }
            r->eax = rd;
            break;
        }
        case SYS_GETPID:
            r->eax = current_process ? current_process->pid : 0;
            break;
        case SYS_EXIT:
            proc_exit();
            break;
        case SYS_YIELD:
            proc_yield();
            break;
        case SYS_UPTIME:
            r->eax = pit_get_uptime();
            break;
        case SYS_SBRK: {
            int32_t increment = (int32_t)a1;
            void* result = proc_sbrk(increment);
            r->eax = (uint32_t)result;
            break;
        }
        case SYS_KILL: {
            uint32_t pid = a1;
            int signum = (int)a2;
            r->eax = (uint32_t)proc_signal(pid, signum);
            break;
        }
        case SYS_SIGNAL: {
            int signum = (int)a1;
            signal_handler_t handler = (signal_handler_t)a2;
            /* SIG_DFL (0) and SIG_IGN (1) are sentinel values, not addresses.
             * A real handler entry point must live in the user region. */
            if (from_user && handler != SIG_DFL && handler != SIG_IGN &&
                !user_range_ok((uint32_t)handler, 1)) {
                r->eax = (uint32_t)-1;
                break;
            }
            r->eax = (uint32_t)proc_sigaction(signum, handler);
            break;
        }
        case SYS_GETSTATS: {
            if (from_user && !user_range_ok(a1, sizeof(system_stats_t))) {
                r->eax = (uint32_t)-1;
                break;
            }
            system_stats_t* s = (system_stats_t*)a1;
            if (s) {
                s->uptime_seconds = pit_get_uptime();
                s->total_ticks = pit_get_ticks();
                s->free_pages = g_pmm_stats.free_pages;
                s->used_pages = g_pmm_stats.used_pages;
                s->heap_allocated = g_heap_stats.total_allocated;
                s->active_processes = g_sched_stats.active_count;
                s->total_ctx_switches = g_sched_stats.total_switches;
                s->total_syscalls = g_syscall_stats.total;
                r->eax = 0;
            } else {
                r->eax = (uint32_t)-1;
            }
            break;
        }
        /* --- ABI v1.1: read-only file access ------------------------------
         * These route through the existing per-process fd table and the VFS.
         * Opens are READ-ONLY (vfs_open == VFS_MODE_READ); there is no write,
         * create, delete or seek syscall, so Ring 3 cannot mutate the VFS. */
        case SYS_OPEN: {
            const char* s = (const char*)a1;
            if (from_user && !user_str_ok(a1, VFS_MAX_NAME)) {
                r->eax = (uint32_t)-1; break;       /* bad path pointer */
            }
            if (!s) { r->eax = (uint32_t)-1; break; }
            /* Copy before the VFS sees it — no TOCTOU on user memory. */
            char path[VFS_MAX_NAME];
            copy_path(path, s);

            int32_t vfd = vfs_open(path);           /* read-only */
            if (vfd < 0) { r->eax = (uint32_t)-2; break; }   /* no such file */
            int32_t fd = proc_alloc_fd(FD_FILE, vfd);
            if (fd < 0) { vfs_close(vfd); r->eax = (uint32_t)-3; break; } /* table full */
            r->eax = (uint32_t)fd;
            break;
        }
        case SYS_FREAD: {
            uint32_t len = a3;
            if (from_user && !user_range_ok(a2, len)) {
                r->eax = (uint32_t)-1; break;       /* bad buffer */
            }
            if (!a2 && len) { r->eax = (uint32_t)-1; break; }
            int32_t vfd = proc_fd_target((int32_t)a1, FD_FILE);
            if (vfd < 0) { r->eax = (uint32_t)-2; break; }   /* bad fd */
            r->eax = (uint32_t)vfs_read(vfd, (void*)a2, len);
            break;
        }
        case SYS_FCLOSE: {
            int32_t vfd = proc_fd_target((int32_t)a1, FD_FILE);
            if (vfd < 0) { r->eax = (uint32_t)-2; break; }   /* bad fd */
            vfs_close(vfd);
            proc_close_fd((int32_t)a1);
            r->eax = 0;
            break;
        }
        default:
            g_syscall_stats.invalid++;
            r->eax = (uint32_t)-1;
    }

    /* Check signals after syscall */
    proc_check_signals();
}

void syscall_init(void) {
    memset(&g_syscall_stats, 0, sizeof(syscall_stats_t));
    idt_register_handler(128, sh);
}

/* User-mode wrappers */
int sys_write(const char* b, uint32_t l) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_WRITE),"b"(b),"c"(l)); return r;
}
int sys_read(char* b, uint32_t l) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_READ),"b"(b),"c"(l)); return r;
}
uint32_t sys_getpid(void) {
    uint32_t r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_GETPID)); return r;
}
void sys_exit(void) {
    __asm__ volatile("int $0x80"::"a"(SYS_EXIT));
}
void sys_yield(void) {
    __asm__ volatile("int $0x80"::"a"(SYS_YIELD));
}
uint32_t sys_uptime(void) {
    uint32_t r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_UPTIME)); return r;
}
int sys_getstats(system_stats_t* s) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_GETSTATS),"b"(s)); return r;
}
void* sys_sbrk(int32_t increment) {
    void* r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_SBRK),"b"(increment)); return r;
}
int sys_kill(uint32_t pid, int signum) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_KILL),"b"(pid),"c"(signum)); return r;
}
int sys_signal(int signum, void (*handler)(int)) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_SIGNAL),"b"(signum),"c"(handler)); return r;
}
int sys_open(const char* path) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_OPEN),"b"(path)); return r;
}
int sys_fread(int fd, char* buf, uint32_t len) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_FREAD),"b"(fd),"c"(buf),"d"(len)); return r;
}
int sys_fclose(int fd) {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(SYS_FCLOSE),"b"(fd)); return r;
}
void syscall_dump(void) {
    vga_puts("  Syscalls: "); vga_put_dec(g_syscall_stats.total); vga_puts(" total\n");
}
