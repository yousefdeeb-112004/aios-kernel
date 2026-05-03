#include <ai/event_bus.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <drivers/pit.h>
#include <drivers/keyboard.h>
#include <lib/string.h>
static ai_event_handler_t subs[AI_EVT_MAX][AI_MAX_SUBSCRIBERS]; static uint32_t scnt[AI_EVT_MAX];
ai_event_stats_t g_ai_event_stats; ai_perf_counters_t g_ai_perf;
static uint32_t psc=0,psw=0,pir=0,pkp=0;
static const char* en[]={"proc_create","proc_exit","ctx_switch","page_alloc","page_free","heap_alloc","heap_free","syscall","file_open","file_read","keypress","timer_tick"};
void ai_init(void){memset(subs,0,sizeof(subs));memset(scnt,0,sizeof(scnt));memset(&g_ai_event_stats,0,sizeof(ai_event_stats_t));memset(&g_ai_perf,0,sizeof(ai_perf_counters_t));g_ai_perf.sample_interval_ticks=PIT_TARGET;}
void ai_subscribe(ai_event_type_t t,ai_event_handler_t h){if(t>=AI_EVT_MAX||scnt[t]>=AI_MAX_SUBSCRIBERS)return;subs[t][scnt[t]++]=h;g_ai_event_stats.total_subscribers++;}
void ai_fire_event(ai_event_type_t t,uint32_t p1,uint32_t p2){if(t>=AI_EVT_MAX)return;g_ai_event_stats.counts[t]++;g_ai_event_stats.total_events++;if(!scnt[t]){g_ai_event_stats.dropped_events++;return;}ai_event_t e={t,pit_get_ticks(),p1,p2};for(uint32_t i=0;i<scnt[t];i++)if(subs[t][i])subs[t][i](&e);}
void ai_sample_perf(void){ai_perf_sample_t*s=&g_ai_perf.samples[g_ai_perf.write_idx];s->tick=pit_get_ticks();s->mem_free_pages=g_pmm_stats.free_pages;s->mem_used_pages=g_pmm_stats.used_pages;s->heap_in_use=g_heap_stats.total_allocated;s->active_processes=g_sched_stats.active_count;s->syscalls_per_sec=g_syscall_stats.total-psc;s->ctx_switches_per_sec=g_sched_stats.total_switches-psw;s->interrupts_per_sec=g_timer_stats.irq_count-pir;s->kb_presses_per_sec=g_kb_stats.total_keypresses-pkp;uint32_t act=s->syscalls_per_sec+s->ctx_switches_per_sec;s->cpu_idle_pct=act==0?95:act<10?80:act<50?50:20;psc=g_syscall_stats.total;psw=g_sched_stats.total_switches;pir=g_timer_stats.irq_count;pkp=g_kb_stats.total_keypresses;g_ai_perf.write_idx=(g_ai_perf.write_idx+1)%AI_PERF_HISTORY;if(g_ai_perf.count<AI_PERF_HISTORY)g_ai_perf.count++;}
ai_perf_sample_t*ai_get_latest_sample(void){if(!g_ai_perf.count)return NULL;return&g_ai_perf.samples[(g_ai_perf.write_idx+AI_PERF_HISTORY-1)%AI_PERF_HISTORY];}
void ai_export_report(void){vga_puts_color("=== AI Report ===\n",VGA_LIGHT_CYAN,VGA_BLACK);ai_perf_sample_t*l=ai_get_latest_sample();if(l){vga_puts("  CPU idle: ~");vga_put_dec(l->cpu_idle_pct);vga_puts("%%  Mem free: ");vga_put_dec(l->mem_free_pages);vga_puts(" pages\n");vga_puts("  Rates/s: sys=");vga_put_dec(l->syscalls_per_sec);vga_puts(" sw=");vga_put_dec(l->ctx_switches_per_sec);vga_puts(" irq=");vga_put_dec(l->interrupts_per_sec);vga_puts(" keys=");vga_put_dec(l->kb_presses_per_sec);vga_puts("\n");}vga_puts("  Events: ");vga_put_dec(g_ai_event_stats.total_events);vga_puts(" Subs: ");vga_put_dec(g_ai_event_stats.total_subscribers);vga_puts(" Samples: ");vga_put_dec(g_ai_perf.count);vga_puts("\n");serial_puts("\n=== AI EXPORT ===\nuptime=");serial_put_dec(pit_get_uptime());serial_puts("\nmem_free=");serial_put_dec(g_pmm_stats.free_pages);serial_puts("\nheap=");serial_put_dec(g_heap_stats.total_allocated);serial_puts("\nevents=");serial_put_dec(g_ai_event_stats.total_events);serial_puts("\n=== END ===\n");}
void ai_dump(void){vga_puts("  AI: ");vga_put_dec(g_ai_event_stats.total_events);vga_puts(" events, ");vga_put_dec(g_ai_perf.count);vga_puts(" samples\n");}
const char*ai_event_name(ai_event_type_t t){if(t>=AI_EVT_MAX)return"?";return en[t];}
