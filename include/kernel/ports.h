#ifndef _KERNEL_PORTS_H
#define _KERNEL_PORTS_H
#include <kernel/types.h>
static inline uint8_t inb(uint16_t port) { uint8_t r; __asm__ volatile("inb %1, %0":"=a"(r):"Nd"(port)); return r; }
static inline void outb(uint16_t port, uint8_t val) { __asm__ volatile("outb %0, %1"::"a"(val),"Nd"(port)); }
static inline void io_wait(void) { outb(0x80, 0); }
#endif
