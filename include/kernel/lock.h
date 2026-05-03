/* =============================================================================
 * Kernel Locking — Interrupt-Safe Critical Sections
 *
 * Provides irq_save/irq_restore for single-core safety, and
 * kernel spinlocks that combine IRQ disable + SMP spinlock.
 *
 * Usage:
 *   uint32_t flags;
 *   irq_save(flags);
 *   ... critical section ...
 *   irq_restore(flags);
 *
 * Or with kernel spinlocks:
 *   static klock_t mylock = KLOCK_INIT;
 *   klock_acquire(&mylock);
 *   ... critical section (IRQs disabled + SMP safe) ...
 *   klock_release(&mylock);
 * ============================================================================= */
#ifndef _KERNEL_LOCK_H
#define _KERNEL_LOCK_H

#include <kernel/types.h>

/* === IRQ save/restore === */

/* Save EFLAGS and disable interrupts */
static inline void irq_save(uint32_t* flags) {
    __asm__ volatile(
        "pushfl\n"
        "popl %0\n"
        "cli"
        : "=r"(*flags)
        :
        : "memory"
    );
}

/* Restore EFLAGS (re-enables interrupts if they were enabled before) */
static inline void irq_restore(uint32_t flags) {
    __asm__ volatile(
        "pushl %0\n"
        "popfl"
        :
        : "r"(flags)
        : "memory"
    );
}

/* Simple disable/enable (when you don't need to save state) */
static inline void irq_disable(void) {
    __asm__ volatile("cli" ::: "memory");
}

static inline void irq_enable(void) {
    __asm__ volatile("sti" ::: "memory");
}

/* Check if interrupts are currently enabled */
static inline bool irq_enabled(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0" : "=r"(flags));
    return (flags & 0x200) != 0; /* IF flag = bit 9 */
}

/* === Kernel Spinlock (IRQ-safe + SMP-safe) === */

typedef struct {
    volatile uint32_t locked;
    uint32_t saved_flags;
} klock_t;

#define KLOCK_INIT { 0, 0 }

/* Acquire: disable IRQs, then spin on lock */
static inline void klock_acquire(klock_t* lock) {
    uint32_t flags;
    irq_save(&flags);
    while (1) {
        uint32_t old;
        __asm__ volatile("xchgl %0, %1"
                         : "=r"(old), "+m"(lock->locked)
                         : "0"(1)
                         : "memory");
        if (old == 0) break;
        /* Briefly re-enable IRQs during spin to prevent deadlock on timers */
        irq_restore(flags);
        __asm__ volatile("pause");
        irq_save(&flags);
    }
    lock->saved_flags = flags;
}

/* Release: unlock and restore IRQ state */
static inline void klock_release(klock_t* lock) {
    uint32_t flags = lock->saved_flags;
    __asm__ volatile("" ::: "memory");
    lock->locked = 0;
    irq_restore(flags);
}

#endif
