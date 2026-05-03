/* =============================================================================
 * SMP (Symmetric Multi-Processing) — Tier 3.3
 *
 * Enables multiple CPU cores using the Local APIC.
 * BSP sends INIT+SIPI IPIs to wake Application Processors.
 *
 * Shell commands (Arabic):
 *   nwat  (نواة nawat = core)     — Show CPU cores + status
 *   qfl   (قفل qufl = lock)      — Test spinlocks across cores
 *   mwzy  (موازي muwazi = parallel) — Parallel workload on all cores
 *
 * QEMU: -smp 4
 * ============================================================================= */
#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

#include <kernel/types.h>

#define MAX_CPUS        8
#define LAPIC_BASE      0xFEE00000

/* Local APIC registers */
#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310

/* ICR bits */
#define ICR_INIT        0x00000500
#define ICR_STARTUP     0x00000600
#define ICR_ASSERT      0x00004000
#define ICR_DEASSERT    0x00000000

/* Spinlock (test-and-set) */
typedef volatile uint32_t spinlock_t;
#define SPINLOCK_INIT   0

/* Per-CPU data */
typedef struct {
    uint8_t  apic_id;
    bool     online;
    bool     is_bsp;
    uint32_t ticks;
    uint32_t work_done;
    uint32_t lock_acquires;
} cpu_data_t;

/* SMP global state */
typedef struct {
    uint32_t   cpu_count;       /* Detected CPUs */
    uint32_t   cpus_online;     /* CPUs that booted */
    bool       apic_available;  /* LAPIC detected */
    bool       smp_enabled;     /* Multi-core active */
    cpu_data_t cpus[MAX_CPUS];
    spinlock_t print_lock;
    /* Parallel work */
    volatile uint32_t shared_counter;
    spinlock_t counter_lock;
} smp_state_t;

extern smp_state_t g_smp;

/* LAPIC MMIO access */
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t val);
uint32_t lapic_id(void);

/* Spinlock */
void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);
bool spin_try_lock(spinlock_t* lock);

/* Init */
void smp_init(void);

/* Shell commands */
void smp_dump(void);
void smp_test_locks(void);
void smp_parallel(void);

#endif
