/* =============================================================================
 * libaios — Ring 3 system call wrappers (implementation)
 *
 * Each wrapper is a single `int $0x80` with the ABI's register assignment.
 * Nothing here touches memory, so none of these needs a stack frame worth
 * speaking of — important, because a user program gets exactly ONE 4KB stack
 * page (see ABI.md "User memory layout").
 *
 * Constraint notes on the inline asm:
 *   - "a" = EAX, "b" = EBX, "c" = ECX, "d" = EDX, matching ABI.md.
 *   - Every wrapper that returns a value marks EAX as an output; the kernel
 *     writes the result into the saved EAX slot before `popal`.
 *   - "memory" is added wherever the kernel may read or write user memory
 *     through a pointer we passed, so the compiler cannot cache those bytes
 *     in registers across the trap.
 * ========================================================================== */

#include "syscalls.h"

int32_t sys_write(const char* buf, uint32_t len) {
    int32_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_WRITE), "b"(buf), "c"(len)
                     : "memory");
    return r;
}

int32_t sys_read(char* buf, uint32_t len) {
    int32_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_READ), "b"(buf), "c"(len)
                     : "memory");
    return r;
}

uint32_t sys_getpid(void) {
    uint32_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_GETPID));
    return r;
}

void sys_exit(void) {
    /* SYS_EXIT takes no arguments in ABI v1 and never returns. The infinite
     * loop is unreachable; it exists so the compiler knows control stops. */
    __asm__ volatile("int $0x80" :: "a"(SYS_EXIT));
    for (;;) { }
}

void sys_yield(void) {
    __asm__ volatile("int $0x80" :: "a"(SYS_YIELD) : "memory");
}

uint32_t sys_uptime(void) {
    uint32_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_UPTIME));
    return r;
}

void* sys_sbrk(int32_t increment) {
    void* r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_SBRK), "b"(increment)
                     : "memory");
    return r;
}

int32_t sys_kill(uint32_t pid, int32_t signum) {
    int32_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_KILL), "b"(pid), "c"(signum));
    return r;
}

int32_t sys_signal(int32_t signum, void (*handler)(int)) {
    int32_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_SIGNAL), "b"(signum), "c"(handler));
    return r;
}

int32_t sys_getstats(sys_stats_t* out) {
    int32_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_GETSTATS), "b"(out)
                     : "memory");
    return r;
}

int32_t sys_open(const char* path) {
    int32_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_OPEN), "b"(path)
                     : "memory");
    return r;
}

int32_t sys_fread(int32_t fd, char* buf, uint32_t len) {
    int32_t r;
    /* The only three-argument syscall in the ABI: length rides in EDX. */
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_FREAD), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return r;
}

int32_t sys_fclose(int32_t fd) {
    int32_t r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_FCLOSE), "b"(fd));
    return r;
}
