/* =============================================================================
 * AI Agent Runtime (Tier 3.2)
 *
 * A rule-based agent with ONE Bayesian (Beta-Bernoulli) node: the memory-leak
 * detector estimates a real conditional probability from sampled evidence,
 * while every other detector remains a hand-tuned threshold rule.
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

    /* --- Beta-Bernoulli memory-leak node (the one learned detector) ---------
     * Posterior over "is memory leaking?" learned from per-window evidence.
     * See ai_agent_check_anomalies() in agent.c ("decision rule v1"). */
    uint32_t ml_alpha;          /* 1 + #windows evidence E held (Laplace)   */
    uint32_t ml_beta;           /* 1 + #windows evidence E did NOT hold      */
    uint32_t ml_consecutive_e;  /* run-length of consecutive E windows       */
    uint32_t ml_windows;        /* windows observed since last decay         */
    uint32_t prev_heap_bytes;   /* heap in-use bytes at the previous window  */
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
