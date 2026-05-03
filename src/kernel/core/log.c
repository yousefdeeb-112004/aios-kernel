#include <kernel/log.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
log_stats_t g_log_stats; static log_level_t mlv=LOG_INFO;
static log_callback_t cbs[MAX_LOG_CALLBACKS]; static int ncb=0; static uint32_t ct=0;
static const char* ln[]={"DEBUG","INFO ","WARN ","ERROR","AI   "};
static const vga_color_t lc[]={VGA_DARK_GREY,VGA_LIGHT_GREEN,VGA_YELLOW,VGA_LIGHT_RED,VGA_LIGHT_CYAN};
void log_init(void){g_log_stats.count_debug=0;g_log_stats.count_info=0;g_log_stats.count_warn=0;g_log_stats.count_error=0;g_log_stats.count_ai=0;g_log_stats.total=0;ncb=0;}
void log_set_tick(uint32_t t){ct=t;}
void klog(log_level_t l,const char*s,const char*m){switch(l){case LOG_DEBUG:g_log_stats.count_debug++;break;case LOG_INFO:g_log_stats.count_info++;break;case LOG_WARN:g_log_stats.count_warn++;break;case LOG_ERROR:g_log_stats.count_error++;break;case LOG_AI:g_log_stats.count_ai++;break;}g_log_stats.total++;serial_puts("[");serial_put_dec(ct);serial_puts("][");serial_puts(ln[l]);serial_puts("] ");serial_puts(s);serial_puts(": ");serial_puts(m);serial_puts("\n");if(l>=mlv){vga_puts("[");vga_puts_color(ln[l],lc[l],VGA_BLACK);vga_puts("] ");vga_puts_color(s,VGA_WHITE,VGA_BLACK);vga_puts(": ");vga_puts(m);vga_puts("\n");}if(ncb>0){log_entry_t e={l,ct,s,m};for(int i=0;i<ncb;i++)if(cbs[i])cbs[i](&e);}}
void log_register_callback(log_callback_t cb){if(ncb<MAX_LOG_CALLBACKS)cbs[ncb++]=cb;}
void log_set_level(log_level_t l){mlv=l;}
void log_dump_stats(void){vga_puts("  Log: I:");vga_put_dec(g_log_stats.count_info);vga_puts(" W:");vga_put_dec(g_log_stats.count_warn);vga_puts(" E:");vga_put_dec(g_log_stats.count_error);vga_puts("\n");}
