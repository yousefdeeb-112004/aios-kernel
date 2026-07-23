/* =============================================================================
 * AI Agent Runtime — Tier 3.2
 *
 * The AI agent observes kernel metrics to detect anomalies, predict actions,
 * and generate recommendations.
 *
 * It is rule-based (threshold comparisons, trend analysis, pattern matching on
 * the Phase 8 data) WITH a small BAYESIAN NETWORK: two evidence nodes (memory-
 * leak and syscall-spike) plus one 2x2 learned CPT (minimal Bayesian network).
 * Each evidence node maintains a real posterior P = alpha/(alpha+beta) learned
 * from sampled evidence; the two of them are the PARENTS of a derived
 * SYSTEM_STRESS node that learns P(stress | E1, E2) as four Beta-Bernoulli
 * cells. All of it shares one bayes_node_t + bayes_update()/bayes_permille(),
 * so the machinery is provably identical across nodes and cells. Every other
 * detector is still a hand-tuned rule. No overclaiming: two evidence nodes +
 * one conditional table.
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

/* Beta-Bernoulli decay period: halve (alpha,beta) every this many windows so a
 * posterior forgets stale evidence (non-stationarity). Shared by BOTH nodes so
 * they forget at exactly the same cadence. */
#define BAYES_DECAY_WINDOWS 64

/* Syscall-spike node: length of the exponential moving average (EMA) used as
 * the robust, self-calibrating baseline for the per-window syscall delta. A
 * power of two so the running mean is a plain shift/divide, no floats. */
#define SC_EMA_N 8

/* SYSTEM_STRESS CPT: EMA length for the cpu_idle_pct baseline the slowdown
 * label compares against. Power of two, same reason as SC_EMA_N. */
#define IDLE_EMA_N 8

/* --- shared Beta-Bernoulli machinery (used by BOTH learned nodes) -----------
 * bayes_update folds one window's binary evidence into a node and then applies
 * the shared exponential-forgetting decay; bayes_permille reports the posterior
 * mean P = alpha/(alpha+beta) in integer permille. This is the whole reason the
 * two nodes cannot drift apart: identical update, identical decay, identical
 * readout — only the evidence variable and the decision rule differ. */
static void bayes_update(bayes_node_t* n, bool evidence) {
    if (evidence) { n->alpha++; n->run_e++; }
    else          { n->beta++;  n->run_e = 0; }

    /* Decay for NON-STATIONARITY: halve both counters (integer division,
     * floored at 1) every BAYES_DECAY_WINDOWS windows — exponential forgetting
     * that preserves the ratio P while keeping the estimator responsive. */
    if (++n->windows >= BAYES_DECAY_WINDOWS) {
        n->windows = 0;
        n->alpha = n->alpha > 1 ? n->alpha / 2 : 1;
        n->beta  = n->beta  > 1 ? n->beta  / 2 : 1;
    }
}

/* P = alpha/(alpha+beta) as fixed-point permille. alpha,beta >= 1 always
 * (Laplace prior), so alpha+beta >= 2 and this never divides by zero. */
static uint32_t bayes_permille(const bayes_node_t* n) {
    return (n->alpha * 1000) / (n->alpha + n->beta);
}

/* Anomaly names */
static const char* anomaly_names[] = {
    "None", "Memory Leak", "High CPU", "Disk Errors",
    "Syscall Spike", "Long Idle", "Heap Fragmented", "Event Storm",
    "System Stress"
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

    /* Beta-Bernoulli prior for BOTH nodes: alpha = beta = 1 (uniform, Laplace
     * smoothing). run_e/windows start at 0 (memset above). */
    g_ai_agent.ml.alpha = 1;
    g_ai_agent.ml.beta  = 1;
    g_ai_agent.sc.alpha = 1;
    g_ai_agent.sc.beta  = 1;

    /* Same uniform prior for all four CPT cells of the SYSTEM_STRESS node. */
    for (int i = 0; i < 4; i++) {
        g_ai_agent.cpt[i].alpha = 1;
        g_ai_agent.cpt[i].beta  = 1;
    }

    /* Seed the syscall baseline at the current total so the FIRST window's delta
     * is ~0 (no spurious spike from syscalls that happened before we started).
     * The EMA baseline itself starts at 0 and calibrates from real deltas. */
    g_ai_agent.prev_syscall_count = g_syscall_stats.total;
    g_ai_agent.sc_ema_scaled      = 0;

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

    bayes_update(&g_ai_agent.ml, evidence);
    uint32_t ml_permille = bayes_permille(&g_ai_agent.ml);

    /* Decision rule v1: raise the Memory Leak anomaly only on SUSTAINED,
     * PROBABLE evidence — at least 3 consecutive E-windows AND a measured
     * posterior P above 60.0%. The trigger adapts: it depends on the run-length
     * of consecutive evidence, not a single fixed threshold, and the 600 gate
     * is on a probability we MEASURED rather than a confidence we invented. */
    if (g_ai_agent.ml.run_e >= 3 && ml_permille > 600) {
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

    /* 4. Syscall spike — Beta-Bernoulli node 2 (the second LEARNED detector) --
     *
     * WHY THIS DETECTOR (and not Event Storm): the syscall count is already
     * sampled per window as a DELTA (g_syscall_stats.total minus last window),
     * which is a natural binary evidence variable — "did this window spike?".
     * Event Storm keys off a CUMULATIVE, monotonically-increasing total
     * (> 1000 events), which trips once and never clears, so it makes a poor
     * per-window Bernoulli trial. The syscall delta is differenced and mean-
     * reverting, so it fits the node cleanly.
     *
     * Evidence variable E2 for this window (integer math only, NO hardcoded
     * absolute threshold): E2 holds iff this window's syscall delta exceeds a
     * baseline the node learns FROM ITS OWN HISTORY — the running mean of
     * previous deltas plus half that mean:
     *     E2 = ( delta > mean + mean/2 )            [ i.e. delta > 1.5 * mean ]
     * where `mean` is an exponential moving average (length SC_EMA_N) of the
     * PRIOR deltas, held scaled by SC_EMA_N in sc_ema_scaled. At idle no user
     * program issues syscalls, so delta and mean are ~0 and E2 is false (low P);
     * a syscall-heavy program makes delta jump far above the learned mean and
     * E2 fires. The margin (1.5x) is relative to the mean, so the trigger
     * rescales itself to whatever "normal" currently is. */
    uint32_t sc_delta = g_syscall_stats.total - g_ai_agent.prev_syscall_count;
    uint32_t sc_mean  = g_ai_agent.sc_ema_scaled / SC_EMA_N;   /* mean of PRIOR deltas */
    bool sc_evidence  = sc_delta > sc_mean + sc_mean / 2;

    bayes_update(&g_ai_agent.sc, sc_evidence);
    uint32_t sc_permille = bayes_permille(&g_ai_agent.sc);

    /* Fold this delta into the EMA baseline AFTER judging it, so the baseline is
     * always the mean of PREVIOUS deltas. Written as (S - S/N + delta) which is
     * always non-negative (S/N <= S) — no unsigned underflow, no floats. */
    g_ai_agent.sc_ema_scaled = g_ai_agent.sc_ema_scaled
                             - g_ai_agent.sc_ema_scaled / SC_EMA_N
                             + sc_delta;

    /* Decision rule v1 (same shape as node 1): raise the Syscall Spike anomaly
     * only on SUSTAINED, PROBABLE evidence — at least 3 consecutive E2-windows
     * AND a measured posterior P above 60.0%. Run-length guards against a single
     * one-off burst; the 600 gate is on a probability we MEASURED. */
    if (g_ai_agent.sc.run_e >= 3 && sc_permille > 600) {
        g_ai_agent.anomalies[ANOMALY_SYSCALL_SPIKE] = true;
        g_ai_agent.anomalies_found++;
    }

    /* 4b. System stress — 2x2 learned CPT (the minimal Bayesian NETWORK) ------
     *
     * The first CONDITIONAL structure: a derived hypothesis P(stress | E1, E2)
     * whose PARENTS are the two evidence nodes above — E1 = memory-leak evidence
     * (`evidence`), E2 = syscall-spike evidence (`sc_evidence`). Instead of one
     * posterior we keep a 2x2 conditional probability table: FOUR independent
     * Beta-Bernoulli cells, one per parent combination (E1,E2) in {00,01,10,11},
     * indexed (E1<<1)|E2. Each window exactly ONE cell — the one selected by
     * this window's parents — is updated, with the SLOWDOWN-PROXY label S below.
     * So each cell learns P(stress | that specific combination): a real
     * conditional, not a marginal.
     *
     * SLOWDOWN PROXY (the label S, from ALREADY-SAMPLED data — no new
     * instrumentation): S holds iff this window's cpu_idle_pct is BELOW its own
     * running mean, i.e. the CPU was busier / slower than its recent baseline.
     * cpu_idle_pct comes from the perf sample (ai_get_latest_sample, taken at
     * the top of ai_agent_analyze); it is a bucketed function of syscall AND
     * context-switch activity, and we compare it to an EMA baseline (length
     * IDLE_EMA_N) so the label is RELATIVE, not a hardcoded absolute. Because it
     * blends context-switch activity too, S is NOT identical to E2 — which is
     * exactly why the four cells learn DIFFERENT conditionals instead of one
     * trivially copying E2.
     *
     * Reported P(stress) for THIS window = the permille of the SELECTED cell.
     * All four cells share the same integer update and the same decay cadence
     * (BAYES_DECAY_WINDOWS, applied per cell by bayes_update). */
    uint32_t idle_pct = sample ? sample->cpu_idle_pct : 95;
    if (g_ai_agent.idle_ema_scaled == 0)                 /* first window: seed */
        g_ai_agent.idle_ema_scaled = idle_pct * IDLE_EMA_N;
    uint32_t idle_mean = g_ai_agent.idle_ema_scaled / IDLE_EMA_N;
    bool stress_label  = idle_pct < idle_mean;           /* busier than baseline */
    g_ai_agent.idle_ema_scaled = g_ai_agent.idle_ema_scaled
                               - g_ai_agent.idle_ema_scaled / IDLE_EMA_N
                               + idle_pct;

    uint32_t cpt_idx = (evidence ? 2u : 0u) | (sc_evidence ? 1u : 0u);  /* (E1<<1)|E2 */
    g_ai_agent.stress_idx = cpt_idx;
    bayes_update(&g_ai_agent.cpt[cpt_idx], stress_label);
    uint32_t stress_permille = bayes_permille(&g_ai_agent.cpt[cpt_idx]);

    /* Run-length of the LABEL (sustained slowdown), analogous to run_e on the
     * evidence nodes but counted across whichever cells the windows land in. */
    if (stress_label) g_ai_agent.stress_run++;
    else              g_ai_agent.stress_run = 0;

    /* Decision rule v1 (same shape as the evidence nodes): raise SYSTEM_STRESS
     * only on SUSTAINED, PROBABLE stress — at least 3 consecutive slowdown
     * windows AND the SELECTED conditional cell's measured P above 60.0%. The
     * gate is on P(stress | current E1,E2), a conditional we MEASURED. */
    if (g_ai_agent.stress_run >= 3 && stress_permille > 600) {
        g_ai_agent.anomalies[ANOMALY_SYSTEM_STRESS] = true;
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

    /* Two Bayesian (Beta-Bernoulli) nodes: show each MEASURED posterior, not a
     * hand-picked confidence. P = alpha/(alpha+beta) rendered as NN.N% from
     * permille, with the raw counts (a,b) and the current evidence run-length. */
    uint32_t ml_permille = bayes_permille(&g_ai_agent.ml);
    vga_puts_color("  MemLeak[Bayes]: ", VGA_WHITE, VGA_BLACK);
    vga_puts("P=");
    vga_put_dec(ml_permille / 10); vga_putchar('.');
    vga_put_dec(ml_permille % 10); vga_puts("%  (a=");
    vga_put_dec(g_ai_agent.ml.alpha); vga_puts(" b=");
    vga_put_dec(g_ai_agent.ml.beta); vga_puts(") runE=");
    vga_put_dec(g_ai_agent.ml.run_e);
    vga_puts("\n");

    uint32_t sc_permille = bayes_permille(&g_ai_agent.sc);
    vga_puts_color("  Syscall[Bayes]: ", VGA_WHITE, VGA_BLACK);
    vga_puts("P=");
    vga_put_dec(sc_permille / 10); vga_putchar('.');
    vga_put_dec(sc_permille % 10); vga_puts("%  (a=");
    vga_put_dec(g_ai_agent.sc.alpha); vga_puts(" b=");
    vga_put_dec(g_ai_agent.sc.beta); vga_puts(") runE=");
    vga_put_dec(g_ai_agent.sc.run_e);
    vga_puts("\n");

    /* SYSTEM_STRESS node: the 2x2 CPT. Show the cell selected by the latest
     * window — its (E1,E2) combination, its a/b, and P(stress|E1,E2) — then a
     * compact dump of all four cells' permilles so the learned differences
     * between conditionals are visible. */
    uint32_t si = g_ai_agent.stress_idx;
    uint32_t sp = bayes_permille(&g_ai_agent.cpt[si]);
    vga_puts_color("  Stress[net]: ", VGA_WHITE, VGA_BLACK);
    vga_puts("E=("); vga_put_dec((si >> 1) & 1u); vga_putchar(',');
    vga_put_dec(si & 1u); vga_puts(")  cell(a=");
    vga_put_dec(g_ai_agent.cpt[si].alpha); vga_puts(" b=");
    vga_put_dec(g_ai_agent.cpt[si].beta); vga_puts(") P=");
    vga_put_dec(sp / 10); vga_putchar('.'); vga_put_dec(sp % 10);
    vga_puts("%  runS="); vga_put_dec(g_ai_agent.stress_run);
    vga_puts("\n");
    vga_puts("    CPT P(permille) 00=");
    vga_put_dec(bayes_permille(&g_ai_agent.cpt[0])); vga_puts(" 01=");
    vga_put_dec(bayes_permille(&g_ai_agent.cpt[1])); vga_puts(" 10=");
    vga_put_dec(bayes_permille(&g_ai_agent.cpt[2])); vga_puts(" 11=");
    vga_put_dec(bayes_permille(&g_ai_agent.cpt[3]));
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
