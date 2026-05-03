#ifndef _KERNEL_DEVTRACK_H
#define _KERNEL_DEVTRACK_H
#include <kernel/types.h>
typedef enum { DEV_IDLE=0,DEV_TYPING,DEV_COMPILING,DEV_DEBUGGING,DEV_EDITING,DEV_UNKNOWN } dev_activity_t;
typedef struct { uint32_t total_compilations; uint32_t last_compile_tick; uint32_t total_compile_ticks; } compile_stats_t;
typedef struct { dev_activity_t current_activity; uint32_t session_start_tick; uint32_t last_keypress_tick; uint32_t last_mouse_tick; uint32_t idle_seconds; uint32_t total_keystrokes; uint32_t total_mouse_events; uint32_t typing_bursts; uint32_t files_opened; uint32_t files_read; compile_stats_t compile; bool break_suggested; } devtrack_stats_t;
extern devtrack_stats_t g_devtrack;
void devtrack_init(void); void devtrack_update(void);
void devtrack_on_keypress(void); void devtrack_on_mouse_move(void);
void devtrack_on_file_open(const char* filename); void devtrack_on_process_create(const char* name);
const char* devtrack_activity_str(void); void devtrack_report(void); void devtrack_dump(void);
#endif
