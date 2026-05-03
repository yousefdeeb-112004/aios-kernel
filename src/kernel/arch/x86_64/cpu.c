#include <kernel/cpu.h>
#include <drivers/vga.h>
#include <lib/string.h>
cpu_info_t g_cpu_info;
static inline void do_cpuid(uint32_t f,uint32_t*a,uint32_t*b,uint32_t*c,uint32_t*d){__asm__ volatile("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(f));}
void cpu_detect(void){uint32_t a,b,c,d;memset(&g_cpu_info,0,sizeof(cpu_info_t));do_cpuid(0,&a,&b,&c,&d);g_cpu_info.max_cpuid=a;*((uint32_t*)&g_cpu_info.vendor[0])=b;*((uint32_t*)&g_cpu_info.vendor[4])=d;*((uint32_t*)&g_cpu_info.vendor[8])=c;g_cpu_info.vendor[12]='\0';if(g_cpu_info.max_cpuid>=1){do_cpuid(1,&a,&b,&c,&d);g_cpu_info.has_fpu=(d>>0)&1;g_cpu_info.has_apic=(d>>9)&1;g_cpu_info.has_sse=(d>>25)&1;g_cpu_info.has_sse2=(d>>26)&1;g_cpu_info.has_pae=(d>>6)&1;}do_cpuid(0x80000000,&a,&b,&c,&d);if(a>=0x80000004){uint32_t*br=(uint32_t*)g_cpu_info.brand;do_cpuid(0x80000002,&br[0],&br[1],&br[2],&br[3]);do_cpuid(0x80000003,&br[4],&br[5],&br[6],&br[7]);do_cpuid(0x80000004,&br[8],&br[9],&br[10],&br[11]);g_cpu_info.brand[48]='\0';}else{strcpy(g_cpu_info.brand,"Unknown");}}
void cpu_dump(void){vga_puts("CPU: ");vga_puts(g_cpu_info.vendor);if(g_cpu_info.has_sse)vga_puts(" SSE");if(g_cpu_info.has_apic)vga_puts(" APIC");if(g_cpu_info.has_pae)vga_puts(" PAE");vga_puts("\n");}
