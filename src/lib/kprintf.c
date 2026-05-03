/* =============================================================================
 * kprintf — Kernel printf implementation
 *
 * Core engine: kvprintf() takes a putchar callback + context pointer.
 * All public functions (kprintf, vga_printf, serial_printf, snprintf)
 * just wrap kvprintf with different callbacks.
 * ============================================================================= */

#include <lib/kprintf.h>
#include <drivers/vga.h>
#include <drivers/serial.h>

/* === kvprintf: the core formatting engine === */

static void put_padding(putchar_fn put, void* ctx, char pad, int count) {
    while (count-- > 0) put(pad, ctx);
}

int kvprintf(putchar_fn put, void* ctx, const char* fmt, va_list ap) {
    int written = 0;

    while (*fmt) {
        if (*fmt != '%') {
            put(*fmt, ctx);
            written++;
            fmt++;
            continue;
        }

        fmt++; /* skip '%' */
        if (!*fmt) break;

        /* Parse flags */
        char pad_char = ' ';
        bool left_align = false;

        if (*fmt == '-') { left_align = true; fmt++; }
        if (*fmt == '0') { pad_char = '0'; fmt++; }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse specifier */
        char spec = *fmt++;
        switch (spec) {
        case 'd': case 'i': {
            int32_t val = va_arg(ap, int32_t);
            char buf[12];
            int i = 0;
            bool neg = false;
            if (val < 0) { neg = true; val = -val; }
            if (val == 0) buf[i++] = '0';
            else { while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; } }
            if (neg) buf[i++] = '-';
            int len = i;
            if (!left_align) put_padding(put, ctx, pad_char, width - len);
            while (i > 0) { put(buf[--i], ctx); written++; }
            if (left_align) put_padding(put, ctx, ' ', width - len);
            written += (width > len) ? width - len : 0;
            break;
        }
        case 'u': {
            uint32_t val = va_arg(ap, uint32_t);
            char buf[12];
            int i = 0;
            if (val == 0) buf[i++] = '0';
            else { while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; } }
            int len = i;
            if (!left_align) put_padding(put, ctx, pad_char, width - len);
            while (i > 0) { put(buf[--i], ctx); written++; }
            if (left_align) put_padding(put, ctx, ' ', width - len);
            written += (width > len) ? width - len : 0;
            break;
        }
        case 'x': case 'X': case 'p': {
            uint32_t val;
            bool prefix = false;
            if (spec == 'p') {
                val = (uint32_t)va_arg(ap, void*);
                prefix = true;
            } else {
                val = va_arg(ap, uint32_t);
            }
            const char* hex = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char buf[10];
            int i = 0;
            if (val == 0) buf[i++] = '0';
            else { while (val > 0) { buf[i++] = hex[val & 0xF]; val >>= 4; } }
            int len = i + (prefix ? 2 : 0);
            if (!left_align) put_padding(put, ctx, pad_char, width - len);
            if (prefix) { put('0', ctx); put('x', ctx); written += 2; }
            while (i > 0) { put(buf[--i], ctx); written++; }
            if (left_align) put_padding(put, ctx, ' ', width - len);
            written += (width > len) ? width - len : 0;
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int len = 0;
            const char* p = s;
            while (*p++) len++;
            if (!left_align) put_padding(put, ctx, ' ', width - len);
            while (*s) { put(*s++, ctx); written++; }
            if (left_align) put_padding(put, ctx, ' ', width - len);
            written += (width > len) ? width - len : 0;
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left_align) put_padding(put, ctx, ' ', width - 1);
            put(c, ctx);
            written++;
            if (left_align) put_padding(put, ctx, ' ', width - 1);
            written += (width > 1) ? width - 1 : 0;
            break;
        }
        case '%':
            put('%', ctx);
            written++;
            break;
        default:
            put('%', ctx);
            put(spec, ctx);
            written += 2;
            break;
        }
    }
    return written;
}

/* === Callbacks for different output targets === */

static void put_vga(char c, void* ctx) {
    (void)ctx;
    vga_putchar(c);
}

static void put_serial(char c, void* ctx) {
    (void)ctx;
    if (c == '\n') serial_putchar('\r');
    serial_putchar(c);
}

static void put_both(char c, void* ctx) {
    (void)ctx;
    vga_putchar(c);
    if (c == '\n') serial_putchar('\r');
    serial_putchar(c);
}

typedef struct {
    char* buf;
    size_t size;
    size_t pos;
} snprintf_ctx_t;

static void put_buf(char c, void* ctx) {
    snprintf_ctx_t* s = (snprintf_ctx_t*)ctx;
    if (s->pos < s->size - 1) {
        s->buf[s->pos] = c;
    }
    s->pos++;
}

/* === Public API === */

void kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(put_both, NULL, fmt, ap);
    va_end(ap);
}

void vga_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(put_vga, NULL, fmt, ap);
    va_end(ap);
}

void serial_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(put_serial, NULL, fmt, ap);
    va_end(ap);
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    if (!buf || size == 0) return 0;
    snprintf_ctx_t ctx = { buf, size, 0 };
    va_list ap;
    va_start(ap, fmt);
    kvprintf(put_buf, &ctx, fmt, ap);
    va_end(ap);
    /* Null-terminate */
    if (ctx.pos < size) buf[ctx.pos] = '\0';
    else buf[size - 1] = '\0';
    return (int)ctx.pos;
}
