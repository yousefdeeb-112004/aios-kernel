#ifndef _KERNEL_PROCESS_H
#define _KERNEL_PROCESS_H
#include <kernel/types.h>
#define MAX_PROCESSES 16
#define PROC_STACK_SIZE 8192

typedef enum { PROC_UNUSED=0, PROC_READY, PROC_RUNNING, PROC_TERMINATED, PROC_STOPPED } proc_state_t;

/* === Signal System === */
#define MAX_SIGNALS   16
#define SIGHUP   1    /* Hangup */
#define SIGINT   2    /* Interrupt (Ctrl+C) */
#define SIGKILL  9    /* Kill (cannot catch/ignore) */
#define SIGUSR1  10   /* User-defined 1 */
#define SIGUSR2  12   /* User-defined 2 */
#define SIGTERM  15   /* Terminate (default kill) */
#define SIGSTOP  17   /* Stop process (cannot catch) */
#define SIGCONT  18   /* Continue stopped process */
#define SIGCHLD  20   /* Child terminated */

#define SIG_DFL  ((signal_handler_t)0)  /* Default: terminate */
#define SIG_IGN  ((signal_handler_t)1)  /* Ignore signal */

typedef void (*signal_handler_t)(int);

/* Per-process AI/stats */
typedef struct {
    uint32_t context_switches;
    uint32_t ticks_used;
    uint32_t created_at_tick;
} proc_ai_t;

/* Per-process memory tracking */
typedef struct {
    uint32_t heap_start;       /* Start of user heap region */
    uint32_t heap_break;       /* Current break (end of used heap) */
    uint32_t heap_max;         /* Maximum break allowed */
    uint32_t total_allocated;  /* Total bytes allocated via sbrk */
    uint32_t sbrk_calls;       /* Number of sbrk calls */
    /* --- Ring 3 heap (processes with a private address space) --------------
     * heap_start/heap_break/heap_max above describe the KERNEL (kmalloc) heap
     * used by kernel-thread sbrk callers. A process running in Ring 3 gets a
     * real user-accessible heap instead, tracked here and backed by PMM frames
     * mapped PTE_USER into its own page directory (see proc_sbrk). */
    uint32_t user_brk;         /* Current Ring 3 break (0 = not initialized) */
    uint32_t user_brk_mapped;  /* Exclusive end of the pages actually mapped  */
} proc_mem_t;

/* Per-process signal state */
typedef struct {
    uint32_t         pending;                   /* Bitmask of pending signals */
    signal_handler_t handlers[MAX_SIGNALS];     /* Per-signal handler (0=DFL, 1=IGN, else=fn) */
    uint32_t         delivered;                 /* Total signals delivered */
    uint32_t         caught;                    /* Total signals caught by handler */
    uint32_t         ignored;                   /* Total signals ignored */
} proc_sig_t;

/* Per-process file descriptor table */
#define PROC_MAX_FDS    16
#define FD_STDIN        0
#define FD_STDOUT       1
#define FD_STDERR       2

typedef enum { FD_NONE=0, FD_CONSOLE, FD_FILE, FD_PIPE } fd_type_t;

typedef struct {
    fd_type_t type;
    int32_t   target;    /* VFS fd, pipe index, or -1 */
    bool      used;
} proc_fd_t;

/* Process control block */
typedef struct {
    uint32_t      pid;
    uint32_t      parent_pid;     /* PID of parent process (0 = no parent) */
    proc_state_t  state;
    char          name[32];
    uint32_t      esp;
    uint32_t      stack_base;
    uint32_t      user_stack;
    uint32_t      priority;
    uint32_t      page_dir;
    uint32_t      cpu_id;
    proc_ai_t     ai;
    proc_mem_t    mem;
    proc_sig_t    sig;
    proc_fd_t     fds[PROC_MAX_FDS];  /* Per-process fd table */
    int32_t       exit_code;
    bool          waited;             /* Has parent collected exit code? */
} process_t;

typedef struct {
    uint32_t total_switches;
    uint32_t total_created;
    uint32_t total_terminated;
    uint32_t total_killed;
    uint32_t active_count;
    uint32_t total_signals;
    uint32_t total_stopped;
} scheduler_stats_t;

extern scheduler_stats_t g_sched_stats;
extern process_t* current_process;

/* Process management */
void proc_init(void);
int32_t proc_create(const char* name, void(*entry)(void), uint32_t priority);
void proc_exit(void);
void proc_yield(void);
void proc_dump(void);
void proc_enable_preemption(void);
int32_t proc_kill(uint32_t pid);
void proc_set_user_stack(uint32_t ustack);
process_t* proc_find(uint32_t pid);

/* Signal system */
int32_t proc_signal(uint32_t pid, int signum);         /* Send signal to process */
int32_t proc_sigaction(int signum, signal_handler_t h); /* Set signal handler for current process */
void    proc_check_signals(void);                       /* Process pending signals (called from scheduler) */
const char* signal_name(int signum);                    /* Get signal name string */

/* sbrk — grow/shrink process heap */
void* proc_sbrk(int32_t increment);

/* waitpid — wait for child to terminate, returns exit code */
int32_t proc_waitpid(uint32_t pid, int32_t* status);

/* Per-process fd table */
void proc_init_fds(process_t* p);   /* Set up stdin/stdout/stderr */
int32_t proc_alloc_fd(fd_type_t type, int32_t target);
void proc_close_fd(int32_t fd);
/* Validate a caller-supplied fd (range + open + expected type) and return its
 * backing target, or -1. Only ever looks at the CALLING process's table. */
int32_t proc_fd_target(int32_t fd, fd_type_t type);

extern void switch_context(uint32_t* old_esp, uint32_t new_esp);
#endif
