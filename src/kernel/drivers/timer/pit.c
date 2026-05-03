#include <drivers/pit.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/log.h>
#include <kernel/ports.h>
timer_stats_t g_timer_stats;
static void th(registers_t*r){(void)r;g_timer_stats.ticks++;g_timer_stats.irq_count++;if(g_timer_stats.ticks%PIT_TARGET==0)g_timer_stats.seconds++;log_set_tick(g_timer_stats.ticks);pic_send_eoi(0);}
void pit_init(void){g_timer_stats.ticks=0;g_timer_stats.seconds=0;g_timer_stats.irq_count=0;g_timer_stats.frequency=PIT_TARGET;uint16_t div=(uint16_t)(PIT_FREQ/PIT_TARGET);outb(PIT_CMD,0x36);outb(PIT_CH0,(uint8_t)(div&0xFF));outb(PIT_CH0,(uint8_t)((div>>8)&0xFF));idt_register_handler(32,th);pic_unmask_irq(0);}
uint32_t pit_get_ticks(void){return g_timer_stats.ticks;}
uint32_t pit_get_uptime(void){return g_timer_stats.seconds;}
void pit_sleep_ms(uint32_t ms){uint32_t t=g_timer_stats.ticks+(ms/10);if(ms%10)t++;while(g_timer_stats.ticks<t)__asm__ volatile("hlt");}
