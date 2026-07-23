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
/* --- ABI v1.1: read-only file access. Appended; numbers 1..10 never move. --- */
#define SYS_OPEN     11  /* EBX=const char* path -> fd, or negative error   */
#define SYS_FREAD    12  /* EBX=fd ECX=buf EDX=len -> bytes read (0 = EOF)  */
#define SYS_FCLOSE   13  /* EBX=fd -> 0, or negative error                  */
/* --- ABI v1.2: read-only AI-agent snapshot. Appended; 1..13 never move. --- */
#define SYS_AISTAT   14  /* EBX=ai_stat_t* out -> 0, or -1 on bad pointer   */
#define SYS_MAX      15
typedef struct { uint32_t counts[SYS_MAX]; uint32_t total; uint32_t invalid; } syscall_stats_t;
typedef struct { uint32_t uptime_seconds; uint32_t total_ticks; uint32_t free_pages; uint32_t used_pages; uint32_t heap_allocated; uint32_t active_processes; uint32_t total_ctx_switches; uint32_t total_syscalls; } system_stats_t;

/* ai_stat_t — a FROZEN, versioned, READ-ONLY snapshot of the AI agent's
 * Bayesian nodes, shared with Ring 3 the same way system_stats_t is (defined
 * here, mirrored manually in src/user/lib/syscalls.h). APPEND-ONLY discipline:
 * new fields are only ever added at the END and AI_STAT_VERSION increments;
 * existing field offsets and meanings never change. version = 1 today. */
#define AI_STAT_VERSION 1
typedef struct {
    uint32_t version;                                  /* = AI_STAT_VERSION      */
    uint32_t n1_alpha, n1_beta, n1_permille, n1_run_e; /* node 1: memory-leak    */
    uint32_t n2_alpha, n2_beta, n2_permille, n2_run_e; /* node 2: syscall-spike  */
    uint32_t anomalies;   /* bit i set iff anomalies[i] active (anomaly_type_t)  */
} ai_stat_t;
extern syscall_stats_t g_syscall_stats;
void syscall_init(void); void syscall_dump(void);
int sys_write(const char* buf, uint32_t len); int sys_read(char* buf, uint32_t len);
uint32_t sys_getpid(void); void sys_exit(void); void sys_yield(void);
uint32_t sys_uptime(void); int sys_getstats(system_stats_t* stats);
void* sys_sbrk(int32_t increment);
int sys_kill(uint32_t pid, int signum);
int sys_signal(int signum, void (*handler)(int));
int sys_open(const char* path);
int sys_fread(int fd, char* buf, uint32_t len);
int sys_fclose(int fd);
#endif
