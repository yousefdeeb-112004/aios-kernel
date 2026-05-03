#ifndef _AI_EVENT_BUS_H
#define _AI_EVENT_BUS_H
#include <kernel/types.h>
typedef enum { AI_EVT_PROCESS_CREATE=0,AI_EVT_PROCESS_EXIT,AI_EVT_CONTEXT_SWITCH,AI_EVT_PAGE_ALLOC,AI_EVT_PAGE_FREE,AI_EVT_HEAP_ALLOC,AI_EVT_HEAP_FREE,AI_EVT_SYSCALL,AI_EVT_FILE_OPEN,AI_EVT_FILE_READ,AI_EVT_KEYPRESS,AI_EVT_TIMER_TICK,AI_EVT_MAX } ai_event_type_t;
typedef struct { ai_event_type_t type; uint32_t tick; uint32_t param1; uint32_t param2; } ai_event_t;
typedef void (*ai_event_handler_t)(const ai_event_t*);
#define AI_MAX_SUBSCRIBERS 8
#define AI_PERF_HISTORY 60
typedef struct { uint32_t tick; uint32_t cpu_idle_pct; uint32_t mem_free_pages; uint32_t mem_used_pages; uint32_t heap_in_use; uint32_t active_processes; uint32_t syscalls_per_sec; uint32_t ctx_switches_per_sec; uint32_t interrupts_per_sec; uint32_t kb_presses_per_sec; } ai_perf_sample_t;
typedef struct { ai_perf_sample_t samples[AI_PERF_HISTORY]; uint32_t write_idx; uint32_t count; uint32_t sample_interval_ticks; } ai_perf_counters_t;
extern ai_perf_counters_t g_ai_perf;
typedef struct { uint32_t counts[AI_EVT_MAX]; uint32_t total_events; uint32_t total_subscribers; uint32_t dropped_events; } ai_event_stats_t;
extern ai_event_stats_t g_ai_event_stats;
void ai_init(void); void ai_fire_event(ai_event_type_t type, uint32_t p1, uint32_t p2);
void ai_subscribe(ai_event_type_t type, ai_event_handler_t handler);
void ai_sample_perf(void); ai_perf_sample_t* ai_get_latest_sample(void);
void ai_export_report(void); void ai_dump(void);
const char* ai_event_name(ai_event_type_t type);
#endif
