#ifndef _DRIVERS_SERIAL_H
#define _DRIVERS_SERIAL_H
#include <kernel/types.h>
#define SERIAL_COM1 0x3F8
void serial_init(void); void serial_putchar(char c); void serial_puts(const char* str);
void serial_put_hex(uint32_t value); void serial_put_dec(uint32_t value);
#endif
