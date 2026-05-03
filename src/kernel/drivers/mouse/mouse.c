/* PS/2 Mouse driver — simple, no scroll wheel */
#include <drivers/mouse.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/ports.h>
#include <drivers/vga.h>
#include <lib/string.h>

mouse_state_t g_mouse;
static uint8_t mc = 0;
static uint8_t mb[3];

static void mww(void) { int t = 100000; while (t-- > 0) if (!(inb(0x64) & 2)) return; }
static void mwr(void) { int t = 100000; while (t-- > 0) if (inb(0x64) & 1) return; }
static void mw(uint8_t d) { mww(); outb(0x64, 0xD4); mww(); outb(0x60, d); }
static uint8_t mr(void) { mwr(); return inb(0x60); }

static void mh(registers_t* r) {
    (void)r;
    uint8_t st = inb(0x64);
    if (!(st & 0x20)) { pic_send_eoi(12); return; }
    uint8_t d = inb(0x60);

    switch (mc) {
    case 0: mb[0] = d; if (d & 0x08) mc = 1; break;
    case 1: mb[1] = d; mc = 2; break;
    case 2:
        mb[2] = d; mc = 0;
        g_mouse.packets_received++;
        bool nl = mb[0] & 1; bool nr2 = mb[0] & 2; bool nm = mb[0] & 4;
        if (nl != g_mouse.left_btn || nr2 != g_mouse.right_btn || nm != g_mouse.middle_btn)
            g_mouse.total_clicks++;
        g_mouse.left_btn = nl; g_mouse.right_btn = nr2; g_mouse.middle_btn = nm;
        int32_t dx = (int32_t)mb[1]; if (mb[0] & 0x10) dx |= 0xFFFFFF00;
        int32_t dy = (int32_t)mb[2]; if (mb[0] & 0x20) dy |= 0xFFFFFF00;
        if (dx || dy) {
            g_mouse.x += dx; g_mouse.y -= dy;
            if (g_mouse.x < 0) g_mouse.x = 0;
            if (g_mouse.x >= VGA_WIDTH * 8) g_mouse.x = VGA_WIDTH * 8 - 1;
            if (g_mouse.y < 0) g_mouse.y = 0;
            if (g_mouse.y >= VGA_HEIGHT * 16) g_mouse.y = VGA_HEIGHT * 16 - 1;
            g_mouse.total_moves++;
        }
        break;
    }
    pic_send_eoi(12);
}

void mouse_init(void) {
    memset(&g_mouse, 0, sizeof(mouse_state_t));
    g_mouse.x = VGA_WIDTH * 4; g_mouse.y = VGA_HEIGHT * 8;
    mc = 0;
    mww(); outb(0x64, 0xA8);
    mww(); outb(0x64, 0x20); mwr();
    uint8_t cfg = inb(0x60); cfg |= 0x02; cfg &= ~0x20;
    mww(); outb(0x64, 0x60); mww(); outb(0x60, cfg);
    mw(0xFF); mr(); mr(); mr();
    mw(0xF6); mr();
    mw(0xF4); mr();
    idt_register_handler(44, mh);
    pic_unmask_irq(12); pic_unmask_irq(2);
}

void mouse_dump(void) {
    vga_puts("  Mouse: ("); vga_put_dec(g_mouse.x); vga_puts(","); vga_put_dec(g_mouse.y);
    vga_puts(") btn:");
    if (g_mouse.left_btn) vga_puts("L");
    if (g_mouse.middle_btn) vga_puts("M");
    if (g_mouse.right_btn) vga_puts("R");
    if (!g_mouse.left_btn && !g_mouse.middle_btn && !g_mouse.right_btn) vga_puts("---");
    vga_puts(" mv:"); vga_put_dec(g_mouse.total_moves);
    vga_puts(" cl:"); vga_put_dec(g_mouse.total_clicks);
    vga_puts("\n");
}
