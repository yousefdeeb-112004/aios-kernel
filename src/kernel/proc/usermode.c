/* =============================================================================
 * User Mode Implementation
 *
 * Demo user-mode programs that run in Ring 3. They can ONLY interact with
 * the kernel via INT 0x80 syscalls (sys_write, sys_getpid, sys_exit, etc.)
 *
 * The transition works like this:
 *   1. Kernel thread allocates a user stack
 *   2. Sets TSS.esp0 to kernel stack (for Ring 3→0 transitions)
 *   3. Calls jump_to_usermode() which does IRET to Ring 3
 *   4. User program runs, making syscalls via INT 0x80
 *   5. User calls sys_exit() → INT 0x80 → handler calls proc_exit()
 *   6. Scheduler returns to kernel/shell thread
 * ============================================================================= */

#include <kernel/usermode.h>
#include <kernel/gdt.h>
#include <kernel/process.h>
#include <kernel/heap.h>
#include <kernel/syscall.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <lib/string.h>

#define USER_STACK_SIZE 4096

/* ==========================================================================
 * USER PROGRAMS — These functions run in Ring 3!
 *
 * They CANNOT call kernel functions directly (would cause GPF).
 * They can ONLY use sys_*() wrappers which internally do INT 0x80.
 * ========================================================================== */

/* User program: Hello from Ring 3 */
static void user_hello(void) {
    /* sys_write goes through INT 0x80 → kernel handles it */
    sys_write("[Ring3] ", 8);

    /* Get our PID via syscall */
    uint32_t pid = sys_getpid();
    char msg[] = "PID=X running in User Mode!\n";
    msg[4] = '0' + (char)(pid % 10);
    sys_write(msg, strlen(msg));

    /* Query system stats via AI syscall */
    system_stats_t stats;
    if (sys_getstats(&stats) == 0) {
        sys_write("[Ring3] Uptime: ", 16);
        /* Simple number print using syscalls */
        char buf[12];
        int i = 0;
        uint32_t v = stats.uptime_seconds;
        if (v == 0) buf[i++] = '0';
        else { while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; } }
        /* Reverse */
        for (int j = 0; j < i/2; j++) {
            char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t;
        }
        sys_write(buf, i);
        sys_write("s, Free mem: ", 13);

        v = (stats.free_pages * 4) / 1024;  /* Convert to MB */
        i = 0;
        if (v == 0) buf[i++] = '0';
        else { while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; } }
        for (int j = 0; j < i/2; j++) {
            char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t;
        }
        sys_write(buf, i);
        sys_write("MB\n", 3);
    }

    sys_write("[Ring3] Exiting user program.\n", 30);

    /* Return to kernel via syscall */
    sys_exit();
}

/* ==========================================================================
 * KERNEL THREAD — Runs in Ring 0, transitions to Ring 3
 * ========================================================================== */

/* This kernel thread sets up Ring 3 and jumps to user_hello */
static void usermode_thread_fn(void) {
    /* Allocate user-mode stack */
    void* ustack = kmalloc(USER_STACK_SIZE);
    if (!ustack) return;

    /* User ESP starts at top of allocated region, with some room */
    uint32_t user_esp = (uint32_t)ustack + USER_STACK_SIZE - 16;

    /* Track user stack for cleanup on process exit */
    proc_set_user_stack((uint32_t)ustack);

    /* Set TSS kernel stack: when INT 0x80 fires from Ring 3,
     * the CPU loads ESP0 from the TSS. Point it to the top of
     * THIS thread's kernel stack (allocated by proc_create). */
    uint32_t kstack_top = current_process->stack_base + PROC_STACK_SIZE;
    tss_set_kernel_stack(kstack_top);

    /* Jump to Ring 3! This never returns.
     * The user program will call sys_exit() which terminates this thread. */
    jump_to_usermode((uint32_t)user_hello, user_esp);

    /* Never reached */
}

void usermode_demo(void) {
    vga_puts_color("=== User Mode Demo ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Launching user program in Ring 3...\n");

    /* Create a kernel thread that will transition to Ring 3 */
    int32_t pid = proc_create("user_ring3", usermode_thread_fn, 128);
    if (pid < 0) {
        vga_puts_color("  Failed to create user process!\n", VGA_LIGHT_RED, VGA_BLACK);
        return;
    }

    vga_puts("  ");

    /* Yield to let the user thread run and complete */
    for (int i = 0; i < 20; i++) {
        proc_yield();
    }

    pit_sleep_ms(200);
    vga_puts("  User program completed. Back in kernel.\n");
}
