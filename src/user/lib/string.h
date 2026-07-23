/* =============================================================================
 * libaios — strings, memory primitives and number formatting
 *
 * Deliberately NOT a printf. The formatting surface here is the minimum a
 * program needs to report results: raw strings, decimal and hex integers.
 * A real formatter can be layered on top later without changing any of this.
 *
 * Stack discipline: every routine below works in place or through
 * caller-supplied buffers. The only stack buffers in the whole library are the
 * fixed 12- and 36-byte scratch arrays inside print_dec/print_hex/print_pad,
 * which is well within the single 4KB user stack page.
 * ========================================================================== */
#ifndef _LIBAIOS_STRING_H
#define _LIBAIOS_STRING_H

#include "types.h"

/* --- memory --------------------------------------------------------------- */
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int   memcmp(const void* a, const void* b, size_t n);

/* --- strings -------------------------------------------------------------- */
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);

/* Bounded copy, strlcpy-style: always NUL-terminates (unless dstsz == 0) and
 * returns the length of `src`, so truncation is detectable by comparing the
 * result against dstsz. Safer than strcpy/strncpy, which is why neither of
 * those exists in this library. */
size_t strlcpy(char* dst, const char* src, size_t dstsz);

/* --- character classes ----------------------------------------------------
 * There is no <ctype.h>. These are ASCII-only and total (no locale, no table),
 * which is all a freestanding parser needs. */
bool is_space(char c);   /* space, tab, CR, LF, form feed, vertical tab */
bool is_digit(char c);   /* '0'..'9' */

/* --- number parsing -------------------------------------------------------
 * Strict decimal parse of an ENTIRE NUL-terminated string, with an optional
 * single leading '-'. Returns true and stores the value only if every
 * character was consumed and the magnitude fits in int32_t; returns false and
 * leaves *out untouched otherwise. An empty string, a lone "-", and trailing
 * garbage all fail — the caller can therefore use a false return as "this
 * token is not a number" without a second pass. */
bool str_to_i32(const char* s, int32_t* out);

/* --- number formatting ----------------------------------------------------
 * Both write into `out` (bounded by outsz, always NUL-terminated) and return
 * the number of characters written, excluding the terminator. `base` must be
 * 10 or 16; anything else formats as 0. No floating point anywhere. */
size_t utoa(uint32_t value, char* out, size_t outsz, uint32_t base);
size_t itoa(int32_t value, char* out, size_t outsz, uint32_t base);

/* --- output over SYS_WRITE ------------------------------------------------ */

/* Writes a NUL-terminated string. */
void print(const char* s);

/* Writes exactly n bytes. NOTE: the kernel's SYS_WRITE stops at the first NUL
 * byte, so this cannot emit binary data containing zeros — use print_hex_bytes
 * for that. */
void print_n(const char* s, size_t n);

void print_dec(int32_t v);          /* signed decimal            */
void print_udec(uint32_t v);        /* unsigned decimal          */
void print_hex(uint32_t v);         /* "0x" + lowercase hex      */

/* Hex-dumps n bytes as "AB CD EF ..." — the NUL-safe way to show binary. */
void print_hex_bytes(const uint8_t* p, size_t n);

#endif /* _LIBAIOS_STRING_H */
