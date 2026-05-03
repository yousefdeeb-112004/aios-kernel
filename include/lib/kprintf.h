/* =============================================================================
 * kprintf — Kernel printf with format strings
 *
 * Supports: %d %u %x %s %c %% %p
 * Outputs to VGA + Serial simultaneously.
 *
 * Usage:
 *   kprintf("Free: %u MB, PID: %d\n", free_mb, pid);
 *   snprintf(buf, sizeof(buf), "Hello %s", name);
 * ============================================================================= */
#ifndef _LIB_KPRINTF_H
#define _LIB_KPRINTF_H

#include <kernel/types.h>

/* Print formatted string to VGA + Serial */
void kprintf(const char* fmt, ...);

/* Print formatted string to VGA only */
void vga_printf(const char* fmt, ...);

/* Print formatted string to Serial only */
void serial_printf(const char* fmt, ...);

/* Print formatted string to buffer (returns chars written) */
int snprintf(char* buf, size_t size, const char* fmt, ...);

/* va_list support (GCC built-in) */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

/* Internal: format into callback */
typedef void (*putchar_fn)(char c, void* ctx);
int kvprintf(putchar_fn put, void* ctx, const char* fmt, va_list ap);

#endif
