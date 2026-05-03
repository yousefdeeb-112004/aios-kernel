/* =============================================================================
 * SMP Implementation
 *
 * 1. Detect APIC via CPUID
 * 2. Map LAPIC MMIO region (0xFEE00000)
 * 3. Search MP Floating Pointer Structure to find CPU count
 * 4. Copy AP trampoline to 0x8000
 * 5. Send INIT+SIPI+SIPI to wake each AP
 * 6. APs execute trampoline → protected mode → C entry
 * 7. Spinlocks via x86 XCHG instruction
 * ============================================================================= */

#include <kernel/smp.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/ports.h>
#include <kernel/cpu.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <lib/string.h>

smp_state_t g_smp;

/* AP trampoline symbols (from ap_trampoline.S) */
extern uint8_t ap_trampoline_start;
extern uint8_t ap_trampoline_end;

/* AP stack area: 4KB per AP */
#define AP_STACK_SIZE 4096
static uint8_t ap_stacks[MAX_CPUS][AP_STACK_SIZE] __attribute__((aligned(4096)));

/* Trampoline load address */
#define TRAMPOLINE_ADDR 0x8000

/* =========================================================================
 * Spinlock — uses x86 XCHG (atomic test-and-set)
 * ========================================================================= */

void spin_lock(spinlock_t* lock) {
    while (1) {
        uint32_t old;
        __asm__ volatile("xchgl %0, %1"
                         : "=r"(old), "+m"(*lock)
                         : "0"(1)
                         : "memory");
        if (old == 0) return;  /* Was unlocked, now locked by us */
        /* Busy wait with PAUSE hint (saves power, avoids pipeline stall) */
        __asm__ volatile("pause");
    }
}

void spin_unlock(spinlock_t* lock) {
    __asm__ volatile("" ::: "memory");  /* Memory barrier */
    *lock = 0;
}

bool spin_try_lock(spinlock_t* lock) {
    uint32_t old;
    __asm__ volatile("xchgl %0, %1"
                     : "=r"(old), "+m"(*lock)
                     : "0"(1)
                     : "memory");
    return (old == 0);
}

/* =========================================================================
 * Local APIC MMIO Access
 * ========================================================================= */

static volatile uint32_t* lapic_base = (volatile uint32_t*)LAPIC_BASE;

uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

uint32_t lapic_id(void) {
    if (!g_smp.apic_available) return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

/* =========================================================================
 * APIC Detection via CPUID
 * ========================================================================= */

static bool detect_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (edx & (1 << 9)) != 0;  /* Bit 9 = APIC present */
}

/* =========================================================================
 * MP Floating Pointer Structure Search
 * Finds how many CPUs the system has.
 * ========================================================================= */

/* MP Floating Pointer signature: "_MP_" */
typedef struct {
    char     signature[4];   /* "_MP_" */
    uint32_t config_ptr;     /* Physical address of MP config table */
    uint8_t  length;         /* In 16-byte units (usually 1) */
    uint8_t  version;        /* MP spec version */
    uint8_t  checksum;
    uint8_t  features[5];
} __attribute__((packed)) mp_float_t;

/* MP Configuration Table Header */
typedef struct {
    char     signature[4];   /* "PCMP" */
    uint16_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[8];
    char     product_id[12];
    uint32_t oem_table;
    uint16_t oem_table_size;
    uint16_t entry_count;
    uint32_t lapic_addr;
    uint16_t ext_table_length;
    uint8_t  ext_table_checksum;
    uint8_t  reserved;
} __attribute__((packed)) mp_config_t;

/* MP Processor Entry */
typedef struct {
    uint8_t  type;           /* 0 = processor */
    uint8_t  apic_id;
    uint8_t  apic_version;
    uint8_t  cpu_flags;      /* Bit 0 = enabled, Bit 1 = BSP */
    uint32_t cpu_signature;
    uint32_t feature_flags;
    uint32_t reserved[2];
} __attribute__((packed)) mp_proc_entry_t;

/* Search for "_MP_" signature in a memory range */
static mp_float_t* mp_search(uint32_t start, uint32_t end) {
    for (uint32_t addr = start; addr < end; addr += 16) {
        mp_float_t* mp = (mp_float_t*)addr;
        if (mp->signature[0] == '_' && mp->signature[1] == 'M' &&
            mp->signature[2] == 'P' && mp->signature[3] == '_') {
            return mp;
        }
    }
    return NULL;
}

static uint32_t mp_find_cpus(void) {
    /* Search standard locations for MP Floating Pointer */
    mp_float_t* mpf = NULL;

    /* 1. First KB of EBDA (Extended BIOS Data Area) */
    uint16_t ebda_seg = *(uint16_t*)0x040E;
    if (ebda_seg) {
        mpf = mp_search((uint32_t)ebda_seg << 4, ((uint32_t)ebda_seg << 4) + 1024);
    }

    /* 2. Last KB of base memory */
    if (!mpf) {
        uint16_t base_kb = *(uint16_t*)0x0413;
        mpf = mp_search((base_kb - 1) * 1024, base_kb * 1024);
    }

    /* 3. BIOS ROM area */
    if (!mpf) {
        mpf = mp_search(0xF0000, 0x100000);
    }

    if (!mpf) return 1;  /* No MP table found, assume 1 CPU */

    /* Parse MP Configuration Table */
    if (mpf->config_ptr == 0) {
        /* No config table — use default config */
        return (mpf->features[0] != 0) ? 2 : 1;
    }

    mp_config_t* mpc = (mp_config_t*)mpf->config_ptr;
    if (mpc->signature[0] != 'P' || mpc->signature[1] != 'C' ||
        mpc->signature[2] != 'M' || mpc->signature[3] != 'P')
        return 1;

    /* Walk entries to count processors */
    uint32_t count = 0;
    uint8_t* entry = (uint8_t*)(mpf->config_ptr + sizeof(mp_config_t));
    for (uint16_t i = 0; i < mpc->entry_count; i++) {
        if (*entry == 0) {  /* Processor entry */
            mp_proc_entry_t* proc = (mp_proc_entry_t*)entry;
            if (proc->cpu_flags & 0x01) {  /* Enabled */
                if (count < MAX_CPUS) {
                    g_smp.cpus[count].apic_id = proc->apic_id;
                    g_smp.cpus[count].is_bsp = (proc->cpu_flags & 0x02) != 0;
                    count++;
                }
            }
            entry += 20;  /* Processor entry is 20 bytes */
        } else {
            /* Skip non-processor entries (I/O APIC, etc) */
            entry += 8;
        }
    }

    return count > 0 ? count : 1;
}

/* =========================================================================
 * AP Entry Point (called by trampoline after switching to pmode)
 * ========================================================================= */

static volatile uint32_t ap_ready_count = 0;
static spinlock_t ap_boot_lock = SPINLOCK_INIT;

/* This function runs on each AP after the trampoline */
void ap_entry(void) {
    /* Read our APIC ID */
    uint32_t id = (lapic_read(LAPIC_ID) >> 24) & 0xFF;

    spin_lock(&ap_boot_lock);

    /* Find our slot and mark online */
    int my_cpu = -1;
    for (uint32_t i = 0; i < g_smp.cpu_count; i++) {
        if (g_smp.cpus[i].apic_id == id) {
            g_smp.cpus[i].online = true;
            my_cpu = (int)i;
            break;
        }
    }

    ap_ready_count++;
    g_smp.cpus_online++;

    spin_unlock(&ap_boot_lock);

    /* Enable LAPIC on this AP */
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0x100);

    /* AP idle loop — participate in parallel work and scheduling */
    while (1) {
        /* Check for parallel work (mwzy command) */
        if (g_smp.shared_counter > 0) {
            spin_lock(&g_smp.counter_lock);
            if (g_smp.shared_counter > 0) {
                g_smp.shared_counter--;
                if (my_cpu >= 0) g_smp.cpus[my_cpu].work_done++;
            }
            spin_unlock(&g_smp.counter_lock);
        }

        /* Track ticks on this AP */
        if (my_cpu >= 0) g_smp.cpus[my_cpu].ticks++;

        __asm__ volatile("pause");
    }
}

/* =========================================================================
 * GDT for AP trampoline (minimal, at known address)
 * ========================================================================= */

/* GDT pointer structure */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_tramp_t;

/* GDT entries for trampoline */
static uint64_t trampoline_gdt[] __attribute__((aligned(8))) = {
    0x0000000000000000ULL,  /* Null */
    0x00CF9A000000FFFFULL,  /* 0x08: Code32 DPL0 */
    0x00CF92000000FFFFULL,  /* 0x10: Data32 DPL0 */
};

/* =========================================================================
 * Send INIT-SIPI-SIPI to wake an AP
 * ========================================================================= */

static void send_ipi(uint8_t apic_id, uint32_t icr_lo) {
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, icr_lo);
    /* Wait for delivery */
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12))
        __asm__ volatile("pause");
}

static void wake_ap(uint8_t apic_id, uint32_t cpu_idx) {
    /* Set up AP stack pointer at 0x8FF0 */
    uint32_t stack_top = (uint32_t)&ap_stacks[cpu_idx][AP_STACK_SIZE - 16];
    *(volatile uint32_t*)0x8FF0 = stack_top;

    /* Set up AP entry point at 0x8FF4 */
    *(volatile uint32_t*)0x8FF4 = (uint32_t)ap_entry;

    /* Send INIT IPI */
    send_ipi(apic_id, ICR_INIT | ICR_ASSERT);
    pit_sleep_ms(10);

    /* Send INIT deassert */
    send_ipi(apic_id, ICR_INIT | ICR_DEASSERT);
    pit_sleep_ms(1);

    /* Send SIPI (Startup IPI) — vector = page number of trampoline */
    uint32_t vector = TRAMPOLINE_ADDR >> 12;  /* 0x8000 >> 12 = 0x08 */
    send_ipi(apic_id, ICR_STARTUP | vector);
    pit_sleep_ms(1);

    /* Send second SIPI (as per Intel spec) */
    send_ipi(apic_id, ICR_STARTUP | vector);
    pit_sleep_ms(5);
}

/* =========================================================================
 * SMP Initialization
 * ========================================================================= */

void smp_init(void) {
    memset(&g_smp, 0, sizeof(smp_state_t));

    /* Check if APIC is available */
    if (!detect_apic()) {
        g_smp.cpu_count = 1;
        g_smp.cpus_online = 1;
        g_smp.cpus[0].online = true;
        g_smp.cpus[0].is_bsp = true;
        g_smp.cpus[0].apic_id = 0;
        return;
    }

    g_smp.apic_available = true;

    /* Map LAPIC MMIO region (0xFEE00000) */
    vmm_map_page(LAPIC_BASE, LAPIC_BASE,
                 PTE_PRESENT | PTE_WRITABLE);

    /* Enable LAPIC on BSP */
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0x100);

    uint32_t bsp_id = lapic_id();

    /* Find CPUs via MP tables */
    g_smp.cpu_count = mp_find_cpus();

    /* If MP tables didn't populate, set BSP manually */
    if (g_smp.cpu_count == 0) {
        g_smp.cpu_count = 1;
    }
    if (!g_smp.cpus[0].online && g_smp.cpu_count >= 1) {
        g_smp.cpus[0].apic_id = bsp_id;
        g_smp.cpus[0].is_bsp = true;
    }

    /* Mark BSP as online */
    for (uint32_t i = 0; i < g_smp.cpu_count; i++) {
        if (g_smp.cpus[i].apic_id == bsp_id) {
            g_smp.cpus[i].online = true;
            g_smp.cpus[i].is_bsp = true;
        }
    }
    g_smp.cpus_online = 1;

    /* If only 1 CPU, nothing more to do */
    if (g_smp.cpu_count <= 1) {
        g_smp.smp_enabled = false;
        return;
    }

    /* Copy trampoline code to 0x8000 */
    uint32_t tramp_size = (uint32_t)&ap_trampoline_end - (uint32_t)&ap_trampoline_start;
    memcpy((void*)TRAMPOLINE_ADDR, &ap_trampoline_start, tramp_size);

    /* Set up GDT pointer at 0x8FF8 for trampoline */
    gdt_ptr_tramp_t* gdtp = (gdt_ptr_tramp_t*)0x8FF8;
    gdtp->limit = sizeof(trampoline_gdt) - 1;
    gdtp->base = (uint32_t)trampoline_gdt;

    /* Wake each AP */
    for (uint32_t i = 0; i < g_smp.cpu_count; i++) {
        if (g_smp.cpus[i].is_bsp) continue;  /* Skip BSP */
        if (g_smp.cpus[i].apic_id == bsp_id) continue;

        wake_ap(g_smp.cpus[i].apic_id, i);
    }

    /* Wait for APs to come online */
    pit_sleep_ms(100);

    g_smp.smp_enabled = (g_smp.cpus_online > 1);
}

/* =========================================================================
 * Shell Commands
 * ========================================================================= */

void smp_dump(void) {
    vga_puts_color("=== CPU Cores (nwat) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  APIC: ");
    vga_puts_color(g_smp.apic_available ? "Available" : "Not detected", 
                   g_smp.apic_available ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("  SMP: ");
    vga_puts_color(g_smp.smp_enabled ? "Active" : "Single-core",
                   g_smp.smp_enabled ? VGA_LIGHT_GREEN : VGA_YELLOW, VGA_BLACK);
    vga_puts("\n  Detected: ");
    vga_put_dec(g_smp.cpu_count);
    vga_puts(" CPUs  Online: ");
    vga_put_dec(g_smp.cpus_online);
    vga_puts("\n\n");

    vga_puts("  ID  BSP  Status    Work   Locks\n");
    for (uint32_t i = 0; i < g_smp.cpu_count; i++) {
        cpu_data_t* c = &g_smp.cpus[i];
        vga_puts("  #"); vga_put_dec(c->apic_id);
        if (c->apic_id < 10) vga_puts(" ");
        vga_puts("  ");
        vga_puts_color(c->is_bsp ? "YES" : "no ", VGA_YELLOW, VGA_BLACK);
        vga_puts("  ");
        vga_puts_color(c->online ? "ONLINE " : "OFFLINE",
                       c->online ? VGA_LIGHT_GREEN : VGA_DARK_GREY, VGA_BLACK);
        vga_puts("  ");
        vga_put_dec(c->work_done);
        if (c->work_done < 10) vga_puts(" ");
        if (c->work_done < 100) vga_puts(" ");
        vga_puts("    ");
        vga_put_dec(c->lock_acquires);
        vga_puts("\n");
    }

    if (!g_smp.smp_enabled && g_smp.cpu_count <= 1) {
        vga_puts_color("\n  Tip: Run QEMU with -smp 4 for multi-core\n", VGA_DARK_GREY, VGA_BLACK);
    }
}

void smp_test_locks(void) {
    vga_puts_color("=== Spinlock Test (qfl) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Testing lock/unlock on BSP...\n");

    spinlock_t test_lock = SPINLOCK_INIT;
    uint32_t iterations = 100000;
    uint32_t count = 0;

    uint32_t start = pit_get_ticks();
    for (uint32_t i = 0; i < iterations; i++) {
        spin_lock(&test_lock);
        count++;
        spin_unlock(&test_lock);
    }
    uint32_t elapsed = pit_get_ticks() - start;

    vga_puts("  Lock/unlock x"); vga_put_dec(iterations);
    vga_puts(": "); vga_put_dec(elapsed); vga_puts(" ticks\n");
    vga_puts("  Final count: "); vga_put_dec(count);
    vga_puts_color(count == iterations ? " [PASS]\n" : " [FAIL]\n",
                   count == iterations ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);

    /* Test try_lock */
    spinlock_t tl = SPINLOCK_INIT;
    bool got1 = spin_try_lock(&tl);
    bool got2 = spin_try_lock(&tl);  /* Should fail — already locked */
    spin_unlock(&tl);
    bool got3 = spin_try_lock(&tl);  /* Should succeed */
    spin_unlock(&tl);

    vga_puts("  try_lock (free): ");
    vga_puts_color(got1 ? "OK" : "FAIL", got1 ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("  (held): ");
    vga_puts_color(!got2 ? "OK" : "FAIL", !got2 ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("  (freed): ");
    vga_puts_color(got3 ? "OK" : "FAIL", got3 ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("\n");

    /* Track BSP lock stats */
    g_smp.cpus[0].lock_acquires += iterations + 3;

    if (g_smp.smp_enabled) {
        vga_puts_color("  Multi-core: ", VGA_WHITE, VGA_BLACK);
        vga_puts("Spinlocks use x86 XCHG (atomic)\n");
        vga_puts("  This ensures only one core holds a lock at a time.\n");
    }
}

void smp_parallel(void) {
    vga_puts_color("=== Parallel Work (mwzy) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  CPUs online: "); vga_put_dec(g_smp.cpus_online); vga_puts("\n");

    /* Reset work counters */
    for (uint32_t i = 0; i < g_smp.cpu_count; i++)
        g_smp.cpus[i].work_done = 0;

    uint32_t total_work = 1000;
    vga_puts("  Distributing "); vga_put_dec(total_work);
    vga_puts(" work units across cores...\n");

    /* Set shared counter — APs will decrement it */
    g_smp.shared_counter = total_work;

    /* BSP also does work */
    uint32_t bsp_done = 0;
    uint32_t start = pit_get_ticks();
    while (g_smp.shared_counter > 0) {
        spin_lock(&g_smp.counter_lock);
        if (g_smp.shared_counter > 0) {
            g_smp.shared_counter--;
            bsp_done++;
        }
        spin_unlock(&g_smp.counter_lock);
        /* Small delay so APs can also grab work */
        if (g_smp.smp_enabled) {
            for (volatile int d = 0; d < 100; d++);
        }
    }
    uint32_t elapsed = pit_get_ticks() - start;

    /* Update BSP work counter */
    for (uint32_t i = 0; i < g_smp.cpu_count; i++) {
        if (g_smp.cpus[i].is_bsp) {
            g_smp.cpus[i].work_done += bsp_done;
            break;
        }
    }

    pit_sleep_ms(50);  /* Wait for APs to finish */

    /* Show results */
    vga_puts("  Completed in "); vga_put_dec(elapsed); vga_puts(" ticks\n\n");
    vga_puts("  Per-core work:\n");
    uint32_t total_done = 0;
    for (uint32_t i = 0; i < g_smp.cpu_count; i++) {
        if (!g_smp.cpus[i].online) continue;
        vga_puts("    CPU #"); vga_put_dec(g_smp.cpus[i].apic_id);
        vga_puts(g_smp.cpus[i].is_bsp ? " (BSP): " : " (AP):  ");
        vga_put_dec(g_smp.cpus[i].work_done);
        vga_puts(" units\n");
        total_done += g_smp.cpus[i].work_done;
    }
    vga_puts("  Total: "); vga_put_dec(total_done);
    vga_puts("/"); vga_put_dec(total_work);
    vga_puts_color(total_done == total_work ? " [PASS]\n" : " [PARTIAL]\n",
                   total_done == total_work ? VGA_LIGHT_GREEN : VGA_YELLOW, VGA_BLACK);
}
