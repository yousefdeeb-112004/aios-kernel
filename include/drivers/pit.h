#ifndef _DRIVERS_PIT_H
#define _DRIVERS_PIT_H
#include <kernel/types.h>
#define PIT_FREQ 1193182
#define PIT_TARGET 100
#define PIT_CMD 0x43
#define PIT_CH0 0x40
typedef struct { uint32_t ticks; uint32_t seconds; uint32_t irq_count; uint32_t frequency; } timer_stats_t;
extern timer_stats_t g_timer_stats;
void pit_init(void); uint32_t pit_get_ticks(void); uint32_t pit_get_uptime(void); void pit_sleep_ms(uint32_t ms);
#endif
