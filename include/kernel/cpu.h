#ifndef _KERNEL_CPU_H
#define _KERNEL_CPU_H
#include <kernel/types.h>
typedef struct { char vendor[16]; char brand[52]; uint32_t family; uint32_t model; uint32_t stepping; bool has_fpu; bool has_sse; bool has_sse2; bool has_apic; bool has_msr; bool has_pae; bool has_pse; uint32_t max_cpuid; } cpu_info_t;
extern cpu_info_t g_cpu_info;
void cpu_detect(void); void cpu_dump(void);
#endif
