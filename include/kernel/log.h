#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H
#include <kernel/types.h>
typedef enum { LOG_DEBUG=0,LOG_INFO=1,LOG_WARN=2,LOG_ERROR=3,LOG_AI=4 } log_level_t;
typedef struct { log_level_t level; uint32_t tick; const char* subsystem; const char* message; } log_entry_t;
typedef void (*log_callback_t)(const log_entry_t*);
typedef struct { uint32_t count_debug; uint32_t count_info; uint32_t count_warn; uint32_t count_error; uint32_t count_ai; uint32_t total; } log_stats_t;
extern log_stats_t g_log_stats;
void log_init(void); void klog(log_level_t level, const char* subsystem, const char* message);
void log_register_callback(log_callback_t cb); void log_set_level(log_level_t min_level);
void log_dump_stats(void); void log_set_tick(uint32_t tick);
#define LOG_INFO_MSG(s,m) klog(LOG_INFO,(s),(m))
#define LOG_WARN_MSG(s,m) klog(LOG_WARN,(s),(m))
#define LOG_ERROR_MSG(s,m) klog(LOG_ERROR,(s),(m))
#define LOG_DEBUG_MSG(s,m) klog(LOG_DEBUG,(s),(m))
#if AI_ENABLED
#define LOG_AI_MSG(s,m) klog(LOG_AI,(s),(m))
#else
#define LOG_AI_MSG(s,m)
#endif
#define MAX_LOG_CALLBACKS 4
#endif
