/* =============================================================================
 * libaios — strings, memory primitives and number formatting (implementation)
 *
 * memcpy/memset are also what GCC lowers struct assignment and array
 * initialisation to even under -ffreestanding -fno-builtin, so they must exist
 * here or the link fails with undefined references.
 * ========================================================================== */

#include "string.h"
#include "syscalls.h"

/* --- memory --------------------------------------------------------------- */

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

/* --- strings -------------------------------------------------------------- */

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
        if (!a[i]) return 0;              /* both hit the terminator */
    }
    return 0;
}

size_t strlcpy(char* dst, const char* src, size_t dstsz) {
    size_t srclen = strlen(src);
    if (dstsz) {
        size_t n = (srclen < dstsz - 1) ? srclen : dstsz - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return srclen;                        /* caller compares against dstsz */
}

/* --- character classes ---------------------------------------------------- */

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* --- number parsing ------------------------------------------------------- */

bool str_to_i32(const char* s, int32_t* out) {
    if (!s || !out) return false;

    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    if (!*s) return false;                  /* a lone "-" is not a number */

    /* Accumulate the magnitude in uint32_t. INT32_MIN's magnitude is
     * 2147483648, which does not fit in int32_t, so the limit check has to be
     * done unsigned and depends on the sign. */
    const uint32_t limit = neg ? 2147483648u : 2147483647u;
    uint32_t mag = 0;
    while (*s) {
        if (!is_digit(*s)) return false;    /* trailing garbage → not a number */
        uint32_t d = (uint32_t)(*s - '0');
        if (mag > (limit - d) / 10u) return false;   /* would overflow */
        mag = mag * 10u + d;
        s++;
    }

    /* 0u - mag reproduces INT32_MIN correctly without signed overflow. */
    *out = neg ? (int32_t)(0u - mag) : (int32_t)mag;
    return true;
}

/* --- number formatting ---------------------------------------------------- */

static const char HEXDIGITS[] = "0123456789abcdef";

size_t utoa(uint32_t value, char* out, size_t outsz, uint32_t base) {
    if (!outsz) return 0;
    if (base != 10 && base != 16) { strlcpy(out, "0", outsz); return 1; }

    /* Build the digits backwards into a scratch buffer, then reverse. 32 bits
     * is at most 10 decimal digits, so 12 bytes is always enough. */
    char tmp[12];
    size_t n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value && n < sizeof(tmp)) {
            tmp[n++] = HEXDIGITS[value % base];
            value /= base;
        }
    }

    size_t written = 0;
    while (n && written < outsz - 1) out[written++] = tmp[--n];
    out[written] = '\0';
    return written;
}

size_t itoa(int32_t value, char* out, size_t outsz, uint32_t base) {
    if (!outsz) return 0;
    if (value >= 0) return utoa((uint32_t)value, out, outsz, base);

    /* Negate through unsigned so INT32_MIN does not overflow: 0u - (uint)v is
     * well defined and yields the correct magnitude. */
    uint32_t mag = 0u - (uint32_t)value;
    if (outsz < 2) { out[0] = '\0'; return 0; }
    out[0] = '-';
    return 1 + utoa(mag, out + 1, outsz - 1, base);
}

/* --- output --------------------------------------------------------------- */

void print(const char* s) {
    sys_write(s, strlen(s));
}

void print_n(const char* s, size_t n) {
    sys_write(s, n);
}

void print_udec(uint32_t v) {
    char buf[12];
    size_t n = utoa(v, buf, sizeof(buf), 10);
    sys_write(buf, n);
}

void print_dec(int32_t v) {
    char buf[13];                          /* sign + 10 digits + NUL */
    size_t n = itoa(v, buf, sizeof(buf), 10);
    sys_write(buf, n);
}

void print_hex(uint32_t v) {
    char buf[11];                          /* "0x" + 8 digits + NUL */
    buf[0] = '0';
    buf[1] = 'x';
    size_t n = utoa(v, buf + 2, sizeof(buf) - 2, 16);
    sys_write(buf, n + 2);
}

void print_hex_bytes(const uint8_t* p, size_t n) {
    /* Flush in small batches so this needs no allocation and only a tiny
     * stack frame: 12 bytes covers four "XX " groups. */
    char buf[12];
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        buf[used++] = HEXDIGITS[(p[i] >> 4) & 0x0F];
        buf[used++] = HEXDIGITS[p[i] & 0x0F];
        buf[used++] = ' ';
        if (used + 3 > sizeof(buf)) { sys_write(buf, used); used = 0; }
    }
    if (used) sys_write(buf, used);
}
