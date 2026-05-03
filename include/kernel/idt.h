#ifndef _KERNEL_IDT_H
#define _KERNEL_IDT_H
#include <kernel/types.h>
typedef struct { uint16_t base_low; uint16_t selector; uint8_t zero; uint8_t flags; uint16_t base_high; } __attribute__((packed)) idt_entry_t;
typedef struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idt_ptr_t;
typedef struct { uint32_t ds; uint32_t edi,esi,ebp,esp_dummy,ebx,edx,ecx,eax; uint32_t int_no,err_code; uint32_t eip,cs,eflags,esp,ss; } __attribute__((packed)) registers_t;
typedef void (*isr_handler_t)(registers_t*);
void idt_init(void); void idt_register_handler(uint8_t n, isr_handler_t handler);
#define IDT_ENTRIES 256
#endif
