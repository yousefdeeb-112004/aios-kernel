/* =============================================================================
 * AI Agent Runtime — Tier 3.2
 *
 * The AI agent observes kernel metrics to detect anomalies, predict actions,
 * and generate recommendations.
 *
 * It is rule-based (threshold comparisons, trend analysis, pattern matching on
 * the Phase 8 data) WITH ONE Bayesian (Beta-Bernoulli) node: the memory-leak
 * detector. That node maintains a real posterior probability learned from
 * sampled evidence — P = alpha/(alpha+beta) — instead of a fixed threshold.
 * Every other detector is still a hand-tuned rule. No overclaiming: one node.
 * ============================================================================= */

#include <ai/agent.h>
#include <ai/event_bus.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <kernel/devtrack.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/ata.h>
#include <drivers/serial.h>
#include <lib/string.h>

ai_agent_t g_ai_agent;

/* Beta-Bernoulli decay period: halve (alpha,beta) every this many windows so
 * the memory-leak posterior forgets stale evidence (non-stationarity). */
#define ML_DECAY_WINDOWS 64

/* Anomaly names */
static const char* anomaly_names[] = {
    "None", "Memory Leak", "High CPU", "Disk Errors",
    "Syscall Spike", "Long Idle", "Heap Fragmented", "Event Storm"
};

/* Prediction names */
static const char* prediction_names[] = {
    "None", "Compile Soon", "Break Needed",
    "File Access", "Idle Soon"
};

/* AI event subscriber — counts events for the agent */
static void agent_event_handler(const ai_event_t* event) {
    (void)event;
    g_ai_agent.events_processed++;
}

void ai_agent_init(void) {
    memset(&g_ai_agent, 0, sizeof(ai_agent_t));
    g_ai_agent.running = true;
    g_ai_agent.start_tick = pit_get_ticks();
    g_ai_agent.current_prediction = PREDICT_NONE;

    /* Beta-Bernoulli prior: alpha = beta = 1 (uniform, Laplace smoothing). */
    g_ai_agent.ml_alpha = 1;
    g_ai_agent.ml_beta  = 1;

    /* Subscribe to all event types */
    ai_subscribe(AI_EVT_PROCESS_CREATE, agent_event_handler);
    ai_subscribe(AI_EVT_PROCESS_EXIT, agent_event_handler);
    ai_subscribe(AI_EVT_SYSCALL, agent_event_handler);
    ai_subscribe(AI_EVT_FILE_OPEN, agent_event_handler);
    ai_subscribe(AI_EVT_KEYPRESS, agent_event_handler);
    ai_subscribe(AI_EVT_HEAP_ALLOC, agent_event_handler);
}

/* === Anomaly Detection === */

void ai_agent_check_anomalies(void) {
    /* Reset all anomalies */
    for (int i = 0; i < ANOMALY_MAX; i++)
        g_ai_agent.anomalies[i] = false;

    /* 1. Memory leak — Beta-Bernoulli node (the one LEARNED detector) ---------
     *
     * Evidence variable E for this sampling window (one per check call):
     *     E = (heap in-use bytes GREW) AND (allocs outpaced frees)
     * Both signals come only from data the agent already samples via
     * g_heap_stats (no new instrumentation): total_allocated is current
     * in-use bytes; alloc_count/free_count are cumulative.
     *
     * We keep a Beta posterior over the leak probability. alpha counts windows
     * where E held, beta where it did not; both start at 1 (Laplace smoothing),
     * so alpha = 1 + #E and beta = 1 + #notE at all times. The reported
     * probability is the posterior mean P = alpha/(alpha+beta), evaluated in
     * fixed-point permille: P_permille = alpha*1000/(alpha+beta). NO floats. */
    uint32_t alloc_delta = g_heap_stats.alloc_count - g_ai_agent.prev_alloc_count;
    uint32_t free_delta  = g_heap_stats.free_count  - g_ai_agent.prev_free_count;
    bool heap_grew = g_heap_stats.total_allocated > g_ai_agent.prev_heap_bytes;
    bool evidence  = heap_grew && (alloc_delta > free_delta);

    if (evidence) {
        g_ai_agent.ml_alpha++;
        g_ai_agent.ml_consecutive_e++;
    } else {
        g_ai_agent.ml_beta++;
        g_ai_agent.ml_consecutive_e = 0;
    }

    /* Decay for NON-STATIONARITY: kernel workload changes over time, so stale
     * evidence must fade or the posterior freezes and stops tracking reality.
     * Every ML_DECAY_WINDOWS windows we halve both counters (integer division,
     * floored at 1) — an exponential forgetting factor that preserves the
     * ratio P while keeping the estimator responsive to recent behavior. */
    if (++g_ai_agent.ml_windows >= ML_DECAY_WINDOWS) {
        g_ai_agent.ml_windows = 0;
        g_ai_agent.ml_alpha = g_ai_agent.ml_alpha > 1 ? g_ai_agent.ml_alpha / 2 : 1;
        g_ai_agent.ml_beta  = g_ai_agent.ml_beta  > 1 ? g_ai_agent.ml_beta  / 2 : 1;
    }

    uint32_t ml_total    = g_ai_agent.ml_alpha + g_ai_agent.ml_beta;
    uint32_t ml_permille = (g_ai_agent.ml_alpha * 1000) / ml_total;

    /* Decision rule v1: raise the Memory Leak anomaly only on SUSTAINED,
     * PROBABLE evidence — at least 3 consecutive E-windows AND a measured
     * posterior P above 60.0%. The trigger adapts: it depends on the run-length
     * of consecutive evidence, not a single fixed threshold, and the 600 gate
     * is on a probability we MEASURED rather than a confidence we invented. */
    if (g_ai_agent.ml_consecutive_e >= 3 && ml_permille > 600) {
        g_ai_agent.anomalies[ANOMALY_MEMORY_LEAK] = true;
        g_ai_agent.anomalies_found++;
    }

    /* 2. High CPU: check latest perf sample */
    ai_perf_sample_t* sample = ai_get_latest_sample();
    if (sample && sample->cpu_idle_pct < 30) {
        g_ai_agent.anomalies[ANOMALY_HIGH_CPU] = true;
        g_ai_agent.anomalies_found++;
    }

    /* 3. Disk errors */
    if (g_ata_disk.present && g_ata_disk.total_errors > g_ai_agent.prev_error_count) {
        g_ai_agent.anomalies[ANOMALY_DISK_ERRORS] = true;
        g_ai_agent.anomalies_found++;
    }

    /* 4. Syscall spike: > 20 syscalls since last check */
    uint32_t syscall_delta = g_syscall_stats.total - g_ai_agent.prev_syscall_count;
    if (syscall_delta > 20) {
        g_ai_agent.anomalies[ANOMALY_SYSCALL_SPIKE] = true;
        g_ai_agent.anomalies_found++;
    }

    /* 5. Long idle */
    if (g_devtrack.idle_seconds > 60) {
        g_ai_agent.anomalies[ANOMALY_LONG_IDLE] = true;
    }

    /* 6. Heap fragmentation: many allocs but alloc_count >> free_count */
    if (g_heap_stats.alloc_count > 10 &&
        g_heap_stats.alloc_count > g_heap_stats.free_count * 3) {
        g_ai_agent.anomalies[ANOMALY_HEAP_FRAGMENTED] = true;
        g_ai_agent.anomalies_found++;
    }

    /* 7. Event storm */
    if (g_ai_event_stats.total_events > 1000) {
        g_ai_agent.anomalies[ANOMALY_EVENT_STORM] = true;
    }

    /* Update previous values for next cycle */
    g_ai_agent.prev_alloc_count = g_heap_stats.alloc_count;
    g_ai_agent.prev_free_count = g_heap_stats.free_count;
    g_ai_agent.prev_syscall_count = g_syscall_stats.total;
    g_ai_agent.prev_error_count = g_ata_disk.total_errors;
    g_ai_agent.prev_heap_bytes = g_heap_stats.total_allocated;
}

/* === Prediction Engine === */

void ai_agent_predict(void) {
    g_ai_agent.current_prediction = PREDICT_NONE;
    g_ai_agent.prediction_confidence = 0;

    devtrack_update();

    /* Rule 1: If developer is typing + files opened recently → compile soon */
    if (g_devtrack.current_activity == DEV_TYPING &&
        g_devtrack.files_opened > 0 &&
        g_devtrack.total_keystrokes > 20) {
        g_ai_agent.current_prediction = PREDICT_COMPILE_SOON;
        g_ai_agent.prediction_confidence = 65;
    }

    /* Rule 2: If developer compiled recently + typing → file access likely */
    if (g_devtrack.compile.total_compilations > 0 &&
        g_devtrack.current_activity == DEV_TYPING) {
        g_ai_agent.current_prediction = PREDICT_FILE_ACCESS;
        g_ai_agent.prediction_confidence = 55;
    }

    /* Rule 3: Long session without break → break needed (overrides others) */
    uint32_t session_sec = (pit_get_ticks() - g_devtrack.session_start_tick) / PIT_TARGET;
    if (session_sec > 1200 && g_devtrack.idle_seconds < 30) {
        g_ai_agent.current_prediction = PREDICT_BREAK_NEEDED;
        g_ai_agent.prediction_confidence = 85;
    }

    /* Rule 4: Decreasing activity → idle soon */
    if (g_devtrack.idle_seconds > 15 && g_devtrack.idle_seconds < 60) {
        if (g_ai_agent.current_prediction == PREDICT_NONE) {
            g_ai_agent.current_prediction = PREDICT_IDLE_SOON;
            g_ai_agent.prediction_confidence = 40;
        }
    }

    if (g_ai_agent.current_prediction != PREDICT_NONE)
        g_ai_agent.predictions_made++;
}

/* === Full Analysis Cycle === */

void ai_agent_analyze(void) {
    if (!g_ai_agent.running) return;

    ai_sample_perf();
    ai_agent_check_anomalies();
    ai_agent_predict();
    g_ai_agent.analysis_count++;

    /* Export to serial for external AI tools */
    serial_puts("[AI_AGENT] analysis=");
    serial_put_dec(g_ai_agent.analysis_count);
    serial_puts(" anomalies=");
    int acount = 0;
    for (int i = 0; i < ANOMALY_MAX; i++)
        if (g_ai_agent.anomalies[i]) acount++;
    serial_put_dec(acount);
    serial_puts(" prediction=");
    serial_puts(prediction_names[g_ai_agent.current_prediction]);
    serial_puts(" conf=");
    serial_put_dec(g_ai_agent.prediction_confidence);
    serial_puts("\n");
}

/* === Shell Commands === */

/* dka — AI agent status */
void ai_agent_status(void) {
    ai_agent_analyze();  /* Run fresh analysis */

    vga_puts_color("=== AI Agent (dka) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Status: ");
    vga_puts_color(g_ai_agent.running ? "ACTIVE" : "STOPPED", VGA_LIGHT_GREEN, VGA_BLACK);
    uint32_t runtime = (pit_get_ticks() - g_ai_agent.start_tick) / PIT_TARGET;
    vga_puts("  Runtime: ");
    vga_put_dec(runtime / 60); vga_puts("m");
    vga_put_dec(runtime % 60); vga_puts("s\n");

    vga_puts("  Analyses: "); vga_put_dec(g_ai_agent.analysis_count);
    vga_puts("  Events: "); vga_put_dec(g_ai_agent.events_processed);
    vga_puts("  Predictions: "); vga_put_dec(g_ai_agent.predictions_made);
    vga_puts("\n");

    /* Current anomalies */
    vga_puts_color("  Anomalies: ", VGA_WHITE, VGA_BLACK);
    bool any = false;
    for (int i = 1; i < ANOMALY_MAX; i++) {
        if (g_ai_agent.anomalies[i]) {
            if (any) vga_puts(", ");
            vga_puts_color(anomaly_names[i], VGA_LIGHT_RED, VGA_BLACK);
            any = true;
        }
    }
    if (!any) vga_puts_color("None", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts("\n");

    /* Memory-leak Bayesian node: show the MEASURED posterior, not a hand-picked
     * confidence. P = alpha/(alpha+beta) rendered as NN.N% from permille. */
    uint32_t ml_total    = g_ai_agent.ml_alpha + g_ai_agent.ml_beta;
    uint32_t ml_permille = (g_ai_agent.ml_alpha * 1000) / ml_total;
    vga_puts_color("  MemLeak[Bayes]: ", VGA_WHITE, VGA_BLACK);
    vga_puts("P=");
    vga_put_dec(ml_permille / 10); vga_putchar('.');
    vga_put_dec(ml_permille % 10); vga_puts("%  (a=");
    vga_put_dec(g_ai_agent.ml_alpha); vga_puts(" b=");
    vga_put_dec(g_ai_agent.ml_beta); vga_puts(") runE=");
    vga_put_dec(g_ai_agent.ml_consecutive_e);
    vga_puts("\n");

    /* Current prediction */
    vga_puts_color("  Prediction: ", VGA_WHITE, VGA_BLACK);
    if (g_ai_agent.current_prediction != PREDICT_NONE) {
        vga_puts_color(prediction_names[g_ai_agent.current_prediction],
                       VGA_YELLOW, VGA_BLACK);
        vga_puts(" ("); vga_put_dec(g_ai_agent.prediction_confidence);
        vga_puts("% confidence)\n");
    } else {
        vga_puts("Observing...\n");
    }
}

/* tnb — Predictions detail */
void ai_agent_predictions(void) {
    ai_agent_predict();

    vga_puts_color("=== AI Predictions (tnb) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);

    devtrack_update();
    uint32_t session = (pit_get_ticks() - g_devtrack.session_start_tick) / PIT_TARGET;

    vga_puts("  Developer state: ");
    vga_puts_color(devtrack_activity_str(), VGA_YELLOW, VGA_BLACK);
    vga_puts("  Session: ");
    vga_put_dec(session / 60); vga_puts("m\n");
    vga_puts("  Keystrokes: "); vga_put_dec(g_devtrack.total_keystrokes);
    vga_puts("  Files: "); vga_put_dec(g_devtrack.files_opened);
    vga_puts("  Compiles: "); vga_put_dec(g_devtrack.compile.total_compilations);
    vga_puts("\n\n");

    /* Prediction reasoning */
    vga_puts_color("  AI thinks:\n", VGA_WHITE, VGA_BLACK);
    switch (g_ai_agent.current_prediction) {
    case PREDICT_COMPILE_SOON:
        vga_puts_color("    > ", VGA_YELLOW, VGA_BLACK);
        vga_puts("You're typing code. Based on keystroke patterns\n");
        vga_puts("      and file access, a compilation is likely soon.\n");
        vga_puts("      Suggestion: Save your work.\n");
        break;
    case PREDICT_BREAK_NEEDED:
        vga_puts_color("    > ", VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("You've been working for over 20 minutes without\n");
        vga_puts("      a significant break. Studies show productivity\n");
        vga_puts("      drops after 25min. Consider a short break.\n");
        break;
    case PREDICT_FILE_ACCESS:
        vga_puts_color("    > ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("After compiling, developers usually check output\n");
        vga_puts("      files or open source files. Predicting file\n");
        vga_puts("      access soon.\n");
        break;
    case PREDICT_IDLE_SOON:
        vga_puts_color("    > ", VGA_DARK_GREY, VGA_BLACK);
        vga_puts("Activity is decreasing. You may be finishing up\n");
        vga_puts("      or about to switch tasks.\n");
        break;
    default:
        vga_puts_color("    > ", VGA_LIGHT_GREY, VGA_BLACK);
        vga_puts("Collecting data... Type commands, open files,\n");
        vga_puts("      use the system — AI learns from your patterns.\n");
        break;
    }

    vga_puts("\n  Confidence: ");
    vga_put_dec(g_ai_agent.prediction_confidence);
    vga_puts("%  [");
    int bars = g_ai_agent.prediction_confidence / 5;
    for (int i = 0; i < 20; i++) {
        if (i < bars) vga_puts_color("#", VGA_LIGHT_GREEN, VGA_BLACK);
        else vga_puts_color("-", VGA_DARK_GREY, VGA_BLACK);
    }
    vga_puts("]\n");
}

/* thll — Deep system analysis */
void ai_agent_deep_analysis(void) {
    ai_agent_analyze();

    vga_puts_color("=== AI Deep Analysis (thll) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);

    /* Memory Analysis */
    vga_puts_color("\n  [Memory]\n", VGA_LIGHT_GREEN, VGA_BLACK);
    uint32_t mem_pct = 0;
    if (g_pmm_stats.total_pages > 0) {
        uint32_t u = g_pmm_stats.used_pages, t = g_pmm_stats.total_pages;
        while (t > 10000) { u >>= 1; t >>= 1; }
        if (t > 0) mem_pct = (u * 100) / t;
    }
    vga_puts("    Usage: "); vga_put_dec(mem_pct); vga_puts("% (");
    vga_put_dec(g_pmm_stats.used_pages); vga_puts("/");
    vga_put_dec(g_pmm_stats.total_pages); vga_puts(" pages)\n");
    if (mem_pct > 80) {
        vga_puts_color("    WARNING: Memory usage above 80%!\n", VGA_LIGHT_RED, VGA_BLACK);
    } else if (mem_pct > 50) {
        vga_puts_color("    Note: Moderate memory usage.\n", VGA_YELLOW, VGA_BLACK);
    } else {
        vga_puts_color("    OK: Memory usage healthy.\n", VGA_LIGHT_GREEN, VGA_BLACK);
    }
    vga_puts("    Heap: "); vga_put_dec(g_heap_stats.total_allocated);
    vga_puts("B in "); vga_put_dec(g_heap_stats.alloc_count - g_heap_stats.free_count);
    vga_puts(" active blocks\n");

    /* Process Analysis */
    vga_puts_color("\n  [Processes]\n", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts("    Active: "); vga_put_dec(g_sched_stats.active_count);
    vga_puts("  Total created: "); vga_put_dec(g_sched_stats.total_created);
    vga_puts("\n    Context switches: "); vga_put_dec(g_sched_stats.total_switches);
    uint32_t up = pit_get_uptime();
    if (up > 0) {
        vga_puts(" ("); vga_put_dec(g_sched_stats.total_switches / up);
        vga_puts("/sec avg)");
    }
    vga_puts("\n");
    if (g_sched_stats.total_switches > up * 100) {
        vga_puts_color("    WARNING: High context switch rate!\n", VGA_YELLOW, VGA_BLACK);
    }

    /* I/O Analysis */
    vga_puts_color("\n  [I/O]\n", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts("    Files: "); vga_put_dec(g_vfs_stats.total_files);
    vga_puts("  Total reads: "); vga_put_dec(g_vfs_stats.total_reads);
    vga_puts("  Bytes: "); vga_put_dec(g_vfs_stats.total_bytes_read); vga_puts("\n");
    vga_puts("    Syscalls: "); vga_put_dec(g_syscall_stats.total);
    vga_puts("  Keyboard: "); vga_put_dec(g_kb_stats.total_keypresses);
    vga_puts(" keys\n");
    if (g_ata_disk.present) {
        vga_puts("    Disk I/O: R="); vga_put_dec(g_ata_disk.total_reads);
        vga_puts(" W="); vga_put_dec(g_ata_disk.total_writes);
        if (g_ata_disk.total_errors > 0) {
            vga_puts_color("  ERRORS=", VGA_LIGHT_RED, VGA_BLACK);
            vga_put_dec(g_ata_disk.total_errors);
        }
        vga_puts("\n");
    }

    /* AI Event Bus Analysis */
    vga_puts_color("\n  [AI Event Bus]\n", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts("    Events: "); vga_put_dec(g_ai_event_stats.total_events);
    vga_puts("  Subscribers: "); vga_put_dec(g_ai_event_stats.total_subscribers);
    vga_puts("\n    Dropped: "); vga_put_dec(g_ai_event_stats.dropped_events);
    if (g_ai_event_stats.dropped_events > 0) {
        vga_puts_color(" (events with no subscribers)", VGA_YELLOW, VGA_BLACK);
    }
    vga_puts("\n    Perf samples: "); vga_put_dec(g_ai_perf.count); vga_puts("/60\n");

    /* Overall Score */
    vga_puts_color("\n  [System Health Score]\n", VGA_WHITE, VGA_BLACK);
    int score = 100;
    int anomaly_count = 0;
    for (int i = 1; i < ANOMALY_MAX; i++)
        if (g_ai_agent.anomalies[i]) anomaly_count++;
    score -= anomaly_count * 15;
    if (mem_pct > 80) score -= 20;
    if (g_ata_disk.total_errors > 0) score -= 10;
    if (score < 0) score = 0;

    vga_puts("    Score: ");
    if (score >= 80)
        vga_puts_color("EXCELLENT", VGA_LIGHT_GREEN, VGA_BLACK);
    else if (score >= 60)
        vga_puts_color("GOOD", VGA_YELLOW, VGA_BLACK);
    else if (score >= 40)
        vga_puts_color("FAIR", VGA_BROWN, VGA_BLACK);
    else
        vga_puts_color("POOR", VGA_LIGHT_RED, VGA_BLACK);
    vga_puts(" ("); vga_put_dec(score); vga_puts("/100)\n");
}

/* rsd — Live monitor */
void ai_agent_monitor(void) {
    vga_puts_color("=== AI Live Monitor (rsd) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Monitoring system... Press any key to stop.\n\n");

    keyboard_set_echo(false);
    uint32_t cycle = 0;

    while (!keyboard_has_char()) {
        ai_agent_analyze();
        cycle++;

        /* Show live stats */
        vga_puts("  ["); vga_put_dec(cycle); vga_puts("] ");
        vga_puts("up:"); vga_put_dec(pit_get_uptime()); vga_puts("s ");
        vga_puts("mem:"); vga_put_dec(g_pmm_stats.free_pages); vga_puts("pg ");
        vga_puts("heap:"); vga_put_dec(g_heap_stats.total_allocated); vga_puts("B ");
        vga_puts("sys:"); vga_put_dec(g_syscall_stats.total); vga_puts(" ");
        vga_puts("keys:"); vga_put_dec(g_kb_stats.total_keypresses); vga_puts(" ");

        /* Show anomalies inline */
        bool any_anomaly = false;
        for (int i = 1; i < ANOMALY_MAX; i++) {
            if (g_ai_agent.anomalies[i]) {
                if (!any_anomaly) vga_puts_color("ALERT:", VGA_LIGHT_RED, VGA_BLACK);
                vga_puts(anomaly_names[i]); vga_puts(" ");
                any_anomaly = true;
            }
        }
        if (!any_anomaly)
            vga_puts_color("OK", VGA_LIGHT_GREEN, VGA_BLACK);

        vga_puts("\n");

        /* Wait ~1 second between samples */
        pit_sleep_ms(1000);
    }

    keyboard_getchar();
    keyboard_set_echo(true);
    vga_puts("  Monitor stopped.\n");
}
