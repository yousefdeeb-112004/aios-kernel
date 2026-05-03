#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H
#include <kernel/types.h>
#define SYS_WRITE    1
#define SYS_READ     2
#define SYS_GETPID   3
#define SYS_EXIT     4
#define SYS_YIELD    5
#define SYS_UPTIME   6
#define SYS_SBRK     7   /* Grow/shrink process heap */
#define SYS_KILL     8   /* Send signal to process */
#define SYS_SIGNAL   9   /* Set signal handler */
#define SYS_GETSTATS 10
#define SYS_MAX      11
typedef struct { uint32_t counts[SYS_MAX]; uint32_t total; uint32_t invalid; } syscall_stats_t;
typedef struct { uint32_t uptime_seconds; uint32_t total_ticks; uint32_t free_pages; uint32_t used_pages; uint32_t heap_allocated; uint32_t active_processes; uint32_t total_ctx_switches; uint32_t total_syscalls; } system_stats_t;
extern syscall_stats_t g_syscall_stats;
void syscall_init(void); void syscall_dump(void);
int sys_write(const char* buf, uint32_t len); int sys_read(char* buf, uint32_t len);
uint32_t sys_getpid(void); void sys_exit(void); void sys_yield(void);
uint32_t sys_uptime(void); int sys_getstats(system_stats_t* stats);
void* sys_sbrk(int32_t increment);
int sys_kill(uint32_t pid, int signum);
int sys_signal(int signum, void (*handler)(int));
#endif
