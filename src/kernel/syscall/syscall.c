#include <kernel/syscall.h>
#include <kernel/idt.h>
#include <kernel/process.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <drivers/vga.h>
#include <drivers/keyboard.h>
#include <drivers/pit.h>
#include <lib/string.h>

syscall_stats_t g_syscall_stats;

static void sh(registers_t* r) {
    uint32_t n = r->eax;
    uint32_t a1 = r->ebx;
    uint32_t a2 = r->ecx;

    g_syscall_stats.total++;
    if (n < SYS_MAX) g_syscall_stats.counts[n]++;

    switch (n) {
        case SYS_WRITE: {
            const char* b = (const char*)a1;
            for (uint32_t i = 0; i < a2 && b[i]; i++)
                vga_putchar(b[i]);
            r->eax = a2;
            break;
        }
        case SYS_READ: {
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
            r->eax = (uint32_t)proc_sigaction(signum, handler);
            break;
        }
        case SYS_GETSTATS: {
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
void syscall_dump(void) {
    vga_puts("  Syscalls: "); vga_put_dec(g_syscall_stats.total); vga_puts(" total\n");
}
