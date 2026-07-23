/* =============================================================================
 * libaios — Ring 3 system call wrappers
 *
 * One C function per entry in the AIOS syscall ABI (see ABI.md, v1.1). These
 * are the USER side of the interface and are completely independent of the
 * kernel-side sys_* helpers in src/kernel/syscall/syscall.c — they just happen
 * to describe the same contract from the other side of `int $0x80`.
 *
 * Calling convention (ABI.md "Invocation"):
 *   EAX = syscall number
 *   EBX = arg1, ECX = arg2, EDX = arg3
 *   EAX = return value; negative means error
 *   All registers except EAX are preserved across the trap.
 *
 * The numbers below are frozen. Never renumber them; new syscalls append.
 * ========================================================================== */
#ifndef _LIBAIOS_SYSCALLS_H
#define _LIBAIOS_SYSCALLS_H

#include "types.h"

/* --- ABI v1 --------------------------------------------------------------- */
#define SYS_WRITE     1
#define SYS_READ      2
#define SYS_GETPID    3
#define SYS_EXIT      4
#define SYS_YIELD     5
#define SYS_UPTIME    6
#define SYS_SBRK      7
#define SYS_KILL      8
#define SYS_SIGNAL    9
#define SYS_GETSTATS 10
/* --- ABI v1.1: read-only file access -------------------------------------- */
#define SYS_OPEN     11
#define SYS_FREAD    12
#define SYS_FCLOSE   13

/* Signal handler sentinels (ABI.md, SYS_SIGNAL row). */
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

/* Filled by sys_getstats(). Field order is fixed by the kernel's
 * system_stats_t — eight uint32_t, see ABI.md. */
typedef struct {
    uint32_t uptime_seconds;
    uint32_t total_ticks;
    uint32_t free_pages;
    uint32_t used_pages;
    uint32_t heap_allocated;
    uint32_t active_processes;
    uint32_t total_ctx_switches;
    uint32_t total_syscalls;
} sys_stats_t;

/* Writes bytes to the VGA console. NOTE: the kernel stops at the first NUL
 * byte in the buffer, so this cannot emit binary data containing zeros —
 * format such data as hex first. Returns the requested count. */
int32_t  sys_write(const char* buf, uint32_t len);

/* Reads up to len bytes from the keyboard; stops early when no character is
 * available. Returns the number of bytes actually read. */
int32_t  sys_read(char* buf, uint32_t len);

uint32_t sys_getpid(void);

/* Terminates the calling process. Takes NO arguments in ABI v1 — there is no
 * exit status. Does not return. */
void     sys_exit(void);

void     sys_yield(void);
uint32_t sys_uptime(void);

/* Grows/shrinks the program break. Returns the PREVIOUS break on success
 * (POSIX semantics) or (void*)-1 on failure — which is what a caller hits at
 * the heap cap (USER_HEAP_MAX). Always check for -1, not for NULL. */
void*    sys_sbrk(int32_t increment);

int32_t  sys_kill(uint32_t pid, int32_t signum);
int32_t  sys_signal(int32_t signum, void (*handler)(int));
int32_t  sys_getstats(sys_stats_t* out);

/* Opens a VFS file READ-ONLY. Returns fd >= 3, or negative:
 *   -1 bad path pointer, -2 no such file, -3 fd table full. */
int32_t  sys_open(const char* path);

/* Reads up to len bytes. Returns bytes read, 0 at EOF, or negative:
 *   -1 bad buffer, -2 bad fd. The length travels in EDX. */
int32_t  sys_fread(int32_t fd, char* buf, uint32_t len);

/* Returns 0, or -2 for a bad fd. */
int32_t  sys_fclose(int32_t fd);

#endif /* _LIBAIOS_SYSCALLS_H */
