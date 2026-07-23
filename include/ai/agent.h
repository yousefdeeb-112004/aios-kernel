/* =============================================================================
 * AI Agent Runtime (Tier 3.2)
 *
 * A rule-based agent with a small BAYESIAN NETWORK: two evidence nodes
 * (memory-leak and syscall-spike, each a Beta-Bernoulli posterior over sampled
 * evidence) feed one derived SYSTEM_STRESS node modelled as a 2x2 learned CPT
 * — P(stress | E1, E2) over four Beta-Bernoulli cells. Every other detector
 * remains a hand-tuned threshold rule.
 *
 * It runs inside the kernel, observing system behavior via the Phase 8 event
 * bus and performance counters. It can:
 *
 *   1. Detect anomalies (memory leak, high error rate, idle too long)
 *   2. Predict developer actions (compile after edit, break after long session)
 *   3. Analyze system patterns (hot files, CPU usage trends, I/O patterns)
 *   4. Generate recommendations
 *   5. Live monitoring with alerts
 *
 * Shell commands (Arabic):
 *   dka   (ذكاء اصطناعي)  — AI agent status + analysis
 *   tnb   (تنبؤ predict)   — Show AI predictions
 *   thll  (تحليل analyze)  — Deep system analysis
 *   rsd   (رصد monitor)    — Live monitor with alerts
 * ============================================================================= */
#ifndef _AI_AGENT_H
#define _AI_AGENT_H

#include <kernel/types.h>

/* Anomaly types the AI can detect */
typedef enum {
    ANOMALY_NONE = 0,
    ANOMALY_MEMORY_LEAK,        /* allocs growing without frees */
    ANOMALY_HIGH_CPU,           /* low idle percentage */
    ANOMALY_DISK_ERRORS,        /* ATA errors detected */
    ANOMALY_SYSCALL_SPIKE,      /* sudden increase in syscalls */
    ANOMALY_LONG_IDLE,          /* developer idle > threshold */
    ANOMALY_HEAP_FRAGMENTED,    /* many allocs, low free block size */
    ANOMALY_EVENT_STORM,        /* too many events per second */
    ANOMALY_SYSTEM_STRESS,      /* derived: P(stress|E1,E2) from the 2x2 CPT */
    ANOMALY_MAX
} anomaly_type_t;

/* Prediction types */
typedef enum {
    PREDICT_NONE = 0,
    PREDICT_COMPILE_SOON,       /* developer likely to compile */
    PREDICT_BREAK_NEEDED,       /* long session, suggest break */
    PREDICT_FILE_ACCESS,        /* predict which file accessed next */
    PREDICT_IDLE_SOON,          /* activity winding down */
    PREDICT_MAX
} prediction_type_t;

/* One Beta-Bernoulli node: a Beta(alpha,beta) posterior over a binary per-window
 * event, updated with Laplace smoothing (alpha=beta=1 prior) and exponential
 * forgetting. Shared by every LEARNED detector so their machinery is identical. */
typedef struct {
    uint32_t alpha;   /* 1 + #windows the evidence held (Laplace smoothing) */
    uint32_t beta;    /* 1 + #windows the evidence did NOT hold             */
    uint32_t run_e;   /* run-length of consecutive evidence windows         */
    uint32_t windows; /* windows observed since the last decay              */
} bayes_node_t;

/* AI Agent state */
typedef struct {
    bool     running;           /* Agent is active */
    uint32_t start_tick;        /* When agent started */
    uint32_t analysis_count;    /* Times analyze() called */
    uint32_t predictions_made;
    uint32_t anomalies_found;
    uint32_t events_processed;

    /* Current anomalies (bitfield) */
    bool anomalies[ANOMALY_MAX];

    /* Current prediction */
    prediction_type_t current_prediction;
    uint32_t prediction_confidence;  /* 0-100% */

    /* Pattern tracking */
    uint32_t prev_alloc_count;
    uint32_t prev_free_count;
    uint32_t prev_syscall_count;
    uint32_t prev_error_count;
    uint32_t idle_streak;       /* Consecutive idle samples */
    uint32_t busy_streak;       /* Consecutive busy samples */

    /* --- Beta-Bernoulli nodes (the LEARNED detectors) -----------------------
     * Two posterior-probability nodes, each P = alpha/(alpha+beta) learned from
     * per-window binary evidence; see ai_agent_check_anomalies() in agent.c
     * ("decision rule v1"). Every OTHER detector stays a hand-tuned rule.
     *   ml — memory leak    (evidence: heap grew AND allocs outpaced frees)
     *   sc — syscall spike  (evidence: this window's syscall delta exceeded a
     *        baseline learned from the running mean of previous deltas) */
    bayes_node_t ml;
    bayes_node_t sc;
    uint32_t prev_heap_bytes;   /* heap in-use bytes at the previous window   */
    uint32_t sc_ema_scaled;     /* SC_EMA_N x running mean of prior sc deltas */

    /* --- Derived hypothesis: SYSTEM_STRESS as a 2x2 learned CPT --------------
     * The first CONDITIONAL structure — a minimal Bayesian network. The two
     * evidence nodes above are the PARENTS; this node learns P(stress|E1,E2) as
     * four Beta-Bernoulli cells, indexed (E1<<1)|E2, one per parent combination.
     * Each window exactly one cell updates with the slowdown-proxy label; the
     * reported P(stress) is that selected cell's posterior. See agent.c. */
    bayes_node_t cpt[4];        /* cells for (E1,E2) in {00,01,10,11}         */
    uint32_t idle_ema_scaled;   /* IDLE_EMA_N x running mean of cpu_idle_pct  */
    uint32_t stress_run;        /* consecutive windows the slowdown label held */
    uint32_t stress_idx;        /* CPT cell selected by the latest window      */
} ai_agent_t;

extern ai_agent_t g_ai_agent;

/* Initialize the AI agent */
void ai_agent_init(void);

/* Run one analysis cycle (call periodically) */
void ai_agent_analyze(void);

/* Generate predictions based on current state */
void ai_agent_predict(void);

/* Check for anomalies */
void ai_agent_check_anomalies(void);

/* Shell commands */
void ai_agent_status(void);     /* dka */
void ai_agent_predictions(void); /* tnb */
void ai_agent_deep_analysis(void); /* thll */
void ai_agent_monitor(void);    /* rsd — live monitor */

#endif
