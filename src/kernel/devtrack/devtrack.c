#include <kernel/devtrack.h>
#include <kernel/log.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <lib/string.h>
devtrack_stats_t g_devtrack;
static bool is_compiler(const char*n){return strcmp(n,"gcc")==0||strcmp(n,"make")==0||strcmp(n,"ld")==0||strcmp(n,"nasm")==0;}
void devtrack_init(void){memset(&g_devtrack,0,sizeof(devtrack_stats_t));g_devtrack.session_start_tick=pit_get_ticks();g_devtrack.current_activity=DEV_IDLE;}
void devtrack_update(void){uint32_t now=pit_get_ticks();uint32_t la=g_devtrack.last_keypress_tick;if(g_devtrack.last_mouse_tick>la)la=g_devtrack.last_mouse_tick;if(la>0&&now>la)g_devtrack.idle_seconds=(now-la)/PIT_TARGET;else g_devtrack.idle_seconds=(now-g_devtrack.session_start_tick)/PIT_TARGET;if(g_devtrack.idle_seconds>30)g_devtrack.current_activity=DEV_IDLE;uint32_t ss=(now-g_devtrack.session_start_tick)/PIT_TARGET;if(ss>1800&&g_devtrack.idle_seconds<60)g_devtrack.break_suggested=true;}
void devtrack_on_keypress(void){g_devtrack.last_keypress_tick=pit_get_ticks();g_devtrack.total_keystrokes++;if(g_devtrack.current_activity==DEV_IDLE)g_devtrack.typing_bursts++;g_devtrack.current_activity=DEV_TYPING;}
void devtrack_on_mouse_move(void){g_devtrack.last_mouse_tick=pit_get_ticks();g_devtrack.total_mouse_events++;}
void devtrack_on_file_open(const char*f){(void)f;g_devtrack.files_opened++;if(g_devtrack.files_opened>5)g_devtrack.current_activity=DEV_EDITING;}
void devtrack_on_process_create(const char*n){if(is_compiler(n)){g_devtrack.compile.total_compilations++;g_devtrack.compile.last_compile_tick=pit_get_ticks();g_devtrack.current_activity=DEV_COMPILING;}}
const char*devtrack_activity_str(void){switch(g_devtrack.current_activity){case DEV_IDLE:return"Idle";case DEV_TYPING:return"Typing";case DEV_COMPILING:return"Compiling";case DEV_DEBUGGING:return"Debugging";case DEV_EDITING:return"Editing";default:return"Unknown";}}
void devtrack_report(void){vga_puts_color("=== Developer Activity ===\n",VGA_LIGHT_CYAN,VGA_BLACK);vga_puts("  Status: ");vga_puts_color(devtrack_activity_str(),VGA_WHITE,VGA_BLACK);uint32_t ss=(pit_get_ticks()-g_devtrack.session_start_tick)/PIT_TARGET;vga_puts("  Session: ");vga_put_dec(ss/60);vga_puts("m");vga_put_dec(ss%60);vga_puts("s  Idle: ");vga_put_dec(g_devtrack.idle_seconds);vga_puts("s\n");vga_puts("  Keys: ");vga_put_dec(g_devtrack.total_keystrokes);vga_puts("  Mouse: ");vga_put_dec(g_devtrack.total_mouse_events);vga_puts("  Files: ");vga_put_dec(g_devtrack.files_opened);vga_puts("  Compiles: ");vga_put_dec(g_devtrack.compile.total_compilations);vga_puts("\n");if(g_devtrack.break_suggested)vga_puts_color("  AI: Take a break!\n",VGA_YELLOW,VGA_BLACK);}
void devtrack_dump(void){vga_puts("  Dev: ");vga_puts(devtrack_activity_str());vga_puts(" keys:");vga_put_dec(g_devtrack.total_keystrokes);vga_puts("\n");}
