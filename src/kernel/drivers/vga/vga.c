/* VGA Text Mode Driver — with Scroll-Back Buffer
 *
 * Adds a 200-line ring buffer that captures lines as they scroll off
 * the top of the screen. Page Up / Page Down browses the history.
 * Any new output auto-returns to the live view.
 */
#include <drivers/vga.h>
#include <kernel/ports.h>
#include <lib/string.h>

static uint16_t* vga_buffer;
static int cursor_x, cursor_y;
static uint8_t current_color;

/* === Scroll-back buffer === */
#define SB_LINES 200
static uint16_t sb_buf[SB_LINES][VGA_WIDTH]; /* Ring buffer of saved lines */
static uint32_t sb_write = 0;   /* Next write slot in ring */
static uint32_t sb_count = 0;   /* Total lines stored (max SB_LINES) */
static int32_t  sb_offset = 0;  /* How many lines scrolled back (0 = live) */
static uint16_t live_screen[VGA_WIDTH * VGA_HEIGHT]; /* Saved live screen */

static uint8_t mkc(vga_color_t fg, vga_color_t bg) { return (uint8_t)(fg | (bg << 4)); }
static uint16_t mke(char c, uint8_t cl) { return (uint16_t)c | ((uint16_t)cl << 8); }

/* Output capture for shell pipes */
static char*    cap_buf = NULL;
static uint32_t cap_max = 0;
static uint32_t cap_len = 0;
static bool     cap_on = false;

static void ucur(void) {
    uint16_t p = (uint16_t)(cursor_y * VGA_WIDTH + cursor_x);
    outb(0x3D4, 14); outb(0x3D5, (uint8_t)(p >> 8));
    outb(0x3D4, 15); outb(0x3D5, (uint8_t)(p & 0xFF));
}

static void hide_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

static void show_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
}

void vga_init(void) {
    vga_buffer = (uint16_t*)VGA_BUFFER;
    cursor_x = 0; cursor_y = 0;
    current_color = mkc(VGA_LIGHT_GREY, VGA_BLACK);
    sb_write = 0; sb_count = 0; sb_offset = 0;
    vga_clear();
}

void vga_clear(void) {
    uint16_t b = mke(' ', current_color);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) vga_buffer[i] = b;
    cursor_x = 0; cursor_y = 0; ucur();
}

void vga_scroll(void) {
    if (cursor_y >= VGA_HEIGHT) {
        /* Save the line about to disappear (row 0) into ring buffer */
        memcpy(sb_buf[sb_write], &vga_buffer[0], VGA_WIDTH * 2);
        sb_write = (sb_write + 1) % SB_LINES;
        if (sb_count < SB_LINES) sb_count++;

        /* Shift everything up */
        for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
            vga_buffer[i] = vga_buffer[i + VGA_WIDTH];

        /* Clear bottom line */
        uint16_t b = mke(' ', current_color);
        for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++)
            vga_buffer[i] = b;
        cursor_y = VGA_HEIGHT - 1;
    }
}

void vga_set_color(vga_color_t fg, vga_color_t bg) { current_color = mkc(fg, bg); }

void vga_putchar(char c) {
    /* Capture output for shell pipes */
    if (cap_on && cap_buf && cap_len < cap_max) {
        cap_buf[cap_len++] = c;
    }

    /* If scrolled back, return to live view on any output */
    if (sb_offset > 0) vga_scroll_reset();

    if (c == '\n') { cursor_x = 0; cursor_y++; }
    else if (c == '\r') { cursor_x = 0; }
    else if (c == '\t') { cursor_x = (cursor_x + 4) & ~3; if (cursor_x >= VGA_WIDTH) { cursor_x = 0; cursor_y++; } }
    else if (c == '\b') { if (cursor_x > 0) { cursor_x--; vga_buffer[cursor_y * VGA_WIDTH + cursor_x] = mke(' ', current_color); } }
    else {
        vga_buffer[cursor_y * VGA_WIDTH + cursor_x] = mke(c, current_color);
        cursor_x++;
        if (cursor_x >= VGA_WIDTH) { cursor_x = 0; cursor_y++; }
    }
    vga_scroll(); ucur();
}

void vga_puts(const char* s) { while (*s) vga_putchar(*s++); }
void vga_puts_color(const char* s, vga_color_t fg, vga_color_t bg) {
    uint8_t o = current_color; current_color = mkc(fg, bg); vga_puts(s); current_color = o;
}
void vga_put_hex(uint32_t v) {
    const char h[] = "0123456789ABCDEF";
    vga_puts("0x");
    for (int i = 28; i >= 0; i -= 4) vga_putchar(h[(v >> i) & 0xF]);
}
void vga_put_dec(uint32_t v) {
    if (v == 0) { vga_putchar('0'); return; }
    char b[12]; int i = 0;
    while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) vga_putchar(b[--i]);
}
void vga_set_cursor(int x, int y) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        cursor_x = x; cursor_y = y; ucur();
    }
}

/* === Scroll-Back Status Bar === */
static void sb_draw_status(void) {
    uint8_t bar_c = mkc(VGA_BLACK, VGA_LIGHT_CYAN);
    int row = VGA_HEIGHT - 1;
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_buffer[row * VGA_WIDTH + x] = mke(' ', bar_c);

    const char* msg = " SCROLL  Shift+Up=Back  Shift+Down/Esc/Enter=Live ";
    int x = 1;
    while (*msg && x < VGA_WIDTH - 20) {
        vga_buffer[row * VGA_WIDTH + x] = mke(*msg, bar_c);
        msg++; x++;
    }

    /* Right side: [offset/total] */
    char pos[24];
    int pi = 0;
    pos[pi++] = '[';
    uint32_t v = (uint32_t)sb_offset;
    char nb[12]; int ni = 0;
    if (v == 0) nb[ni++] = '0';
    else { while (v > 0) { nb[ni++] = '0' + (v % 10); v /= 10; } }
    for (int j = ni - 1; j >= 0; j--) pos[pi++] = nb[j];
    pos[pi++] = '/';
    v = sb_count; ni = 0;
    if (v == 0) nb[ni++] = '0';
    else { while (v > 0) { nb[ni++] = '0' + (v % 10); v /= 10; } }
    for (int j = ni - 1; j >= 0; j--) pos[pi++] = nb[j];
    pos[pi++] = ']';
    pos[pi] = '\0';

    int px = VGA_WIDTH - pi - 2;
    for (int i = 0; pos[i] && px + i < VGA_WIDTH; i++)
        vga_buffer[row * VGA_WIDTH + px + i] = mke(pos[i], bar_c);
}

/* === Scroll-Back Rendering === */
static void sb_render(void) {
    int view_rows = VGA_HEIGHT - 1; /* Bottom row = status */
    uint16_t blank = mke(' ', mkc(VGA_LIGHT_GREY, VGA_BLACK));

    for (int row = 0; row < view_rows; row++) {
        /*
         * Virtual line space:
         *   0 .. sb_count-1            = scroll-back ring (oldest first)
         *   sb_count .. sb_count+24    = live screen rows
         *
         * View window starts at: total_lines - sb_offset - view_rows
         * where total_lines = sb_count + VGA_HEIGHT
         */
        int32_t total = (int32_t)sb_count + VGA_HEIGHT;
        int32_t start = total - sb_offset - view_rows;
        int32_t vline = start + row;

        if (vline < 0) {
            for (int x = 0; x < VGA_WIDTH; x++)
                vga_buffer[row * VGA_WIDTH + x] = blank;
        } else if (vline < (int32_t)sb_count) {
            /* From scroll-back ring */
            int32_t ri = (int32_t)sb_write - (int32_t)sb_count + vline;
            if (ri < 0) ri += SB_LINES;
            memcpy(&vga_buffer[row * VGA_WIDTH],
                   sb_buf[ri % SB_LINES], VGA_WIDTH * 2);
        } else {
            /* From saved live screen */
            int32_t lr = vline - (int32_t)sb_count;
            if (lr >= 0 && lr < VGA_HEIGHT)
                memcpy(&vga_buffer[row * VGA_WIDTH],
                       &live_screen[lr * VGA_WIDTH], VGA_WIDTH * 2);
            else
                for (int x = 0; x < VGA_WIDTH; x++)
                    vga_buffer[row * VGA_WIDTH + x] = blank;
        }
    }
    sb_draw_status();
    hide_cursor();
}

/* Page Up */
void vga_scroll_back(void) {
    if (sb_count == 0) return;
    if (sb_offset == 0) {
        /* First scroll: snapshot the live screen */
        memcpy(live_screen, vga_buffer, VGA_WIDTH * VGA_HEIGHT * 2);
    }
    sb_offset += (VGA_HEIGHT / 2);
    if (sb_offset > (int32_t)sb_count) sb_offset = (int32_t)sb_count;
    sb_render();
}

/* Page Down */
void vga_scroll_forward(void) {
    if (sb_offset <= 0) return;
    sb_offset -= (VGA_HEIGHT / 2);
    if (sb_offset <= 0) { sb_offset = 0; vga_scroll_reset(); return; }
    sb_render();
}

/* Return to live view */
void vga_scroll_reset(void) {
    if (sb_offset == 0) return;
    memcpy(vga_buffer, live_screen, VGA_WIDTH * VGA_HEIGHT * 2);
    sb_offset = 0;
    show_cursor();
    ucur();
}

bool vga_is_scrolled(void) { return sb_offset > 0; }

/* === Output Capture for Shell Pipes === */

void vga_capture_start(char* buf, uint32_t max_len) {
    cap_buf = buf;
    cap_max = max_len > 0 ? max_len - 1 : 0;
    cap_len = 0;
    cap_on = true;
    if (buf) buf[0] = '\0';
}

uint32_t vga_capture_stop(void) {
    cap_on = false;
    if (cap_buf && cap_len < cap_max) cap_buf[cap_len] = '\0';
    uint32_t len = cap_len;
    cap_buf = NULL;
    cap_len = 0;
    cap_max = 0;
    return len;
}

bool vga_is_capturing(void) { return cap_on; }
