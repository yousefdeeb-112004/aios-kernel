/* =============================================================================
 * Window Manager — Text-Mode Compositing
 *
 * Renders overlapping windows to the VGA text buffer at 0xB8000.
 * Windows are drawn back-to-front (painter's algorithm).
 * Mouse interaction: click to focus, drag title to move, [X] to close.
 * ============================================================================= */

#include <kernel/wm.h>
#include <kernel/ports.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <kernel/net.h>
#include <kernel/pci.h>
#include <kernel/boot_info.h>
#include <kernel/cpu.h>
#include <kernel/devtrack.h>
#include <ai/event_bus.h>
#include <ai/agent.h>
#include <drivers/vga.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/pit.h>
#include <drivers/ata.h>
#include <lib/string.h>

wm_state_t g_wm;

/* Screen buffer */
#define VRAM ((uint16_t*)0xB8000)
static uint16_t saved_screen[WM_SCREEN_W * WM_SCREEN_H];
static uint16_t screen[WM_SCREEN_W * WM_SCREEN_H];

#define C(fg, bg) ((uint8_t)((fg) | ((bg) << 4)))

static void scr_cell(int x, int y, char ch, uint8_t color) {
    if (x >= 0 && x < WM_SCREEN_W && y >= 0 && y < WM_SCREEN_H)
        screen[y * WM_SCREEN_W + x] = (uint16_t)((uint8_t)ch) | ((uint16_t)color << 8);
}

static void scr_text(int x, int y, const char* s, uint8_t color) {
    while (*s && x < WM_SCREEN_W) { scr_cell(x++, y, *s++, color); }
}

static void scr_num(int x, int y, uint32_t v, uint8_t color) {
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; } }
    char r[12]; for (int j = 0; j < i; j++) r[j] = b[i-1-j]; r[i] = 0;
    scr_text(x, y, r, color);
}

static void scr_fill(int x, int y, int w, int h, char ch, uint8_t color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            scr_cell(x + i, y + j, ch, color);
}

static void scr_hline(int x, int y, int w, uint8_t ch, uint8_t color) {
    for (int i = 0; i < w; i++) scr_cell(x + i, y, ch, color);
}

/* Draw a bordered window frame */
static void draw_window(wm_window_t* win, bool focused) {
    int x = win->x, y = win->y, w = win->w, h = win->h;
    uint8_t bc = focused ? C(15, win->color) : C(7, 0);
    uint8_t tc = focused ? C(15, win->color) : C(0, 7);
    uint8_t cc = focused ? C(0, win->color) : C(0, 8);

    /* Title bar */
    scr_fill(x, y, w, 1, ' ', tc);
    scr_text(x + 1, y, win->title, tc);
    /* Close button [X] */
    scr_cell(x + w - 3, y, '[', focused ? C(14, win->color) : C(7, 7));
    scr_cell(x + w - 2, y, 'X', focused ? C(12, win->color) : C(4, 7));
    scr_cell(x + w - 1, y, ']', focused ? C(14, win->color) : C(7, 7));

    /* Content area */
    scr_fill(x, y + 1, w, h - 1, ' ', cc);

    /* Borders */
    for (int j = 1; j < h; j++) {
        scr_cell(x, y + j, 179, bc);           /* │ left */
        scr_cell(x + w - 1, y + j, 179, bc);   /* │ right */
    }
    scr_hline(x, y + h, w, 196, bc);           /* ─ bottom */
    scr_cell(x, y + h, 192, bc);               /* └ */
    scr_cell(x + w - 1, y + h, 217, bc);       /* ┘ */
}

/* === App Content Renderers === */

static void render_about(wm_window_t* w) {
    int x = w->x + 1, y = w->y + 1;
    uint8_t c = C(0, w->color);
    uint8_t wh = C(15, w->color);
    uint8_t cy = C(11, w->color);
    uint8_t ye = C(14, w->color);
    scr_text(x, y,   "AIOS Kernel v1.6", wh);
    scr_text(x, y+1, "AI Operating System", cy);
    scr_text(x, y+2, "by AL_Yousef_Deeb", ye);
    scr_text(x, y+4, "Phases: 9 core + T2 + T3", C(7, w->color));
    scr_text(x, y+5, "Features:", C(10, w->color));
    scr_text(x, y+6, " Shell ELF Disk Net AI", C(7, w->color));
    scr_text(x, y+7, " UserMode PCI Windows", C(7, w->color));
}

static void render_sysmon(wm_window_t* w) {
    int x = w->x + 1, y = w->y + 1;
    uint8_t lb = C(0, w->color);
    uint8_t hi = C(15, w->color);
    uint8_t gr = C(10, w->color);

    scr_text(x, y, "CPU:", lb); scr_text(x+5, y, g_cpu_info.vendor, hi);
    scr_text(x, y+1, "RAM:", lb);
    scr_num(x+5, y+1, g_boot_info.total_memory_kb/1024, gr);
    scr_text(x+9, y+1, "MB", lb);
    scr_text(x, y+2, "Free:", lb);
    scr_num(x+6, y+2, (g_pmm_stats.free_pages*4)/1024, gr);
    scr_text(x+10, y+2, "MB", lb);
    scr_text(x, y+3, "Heap:", lb);
    scr_num(x+6, y+3, g_heap_stats.total_allocated, C(14, w->color));
    scr_text(x+14, y+3, "B", lb);
    scr_text(x, y+4, "Procs:", lb);
    scr_num(x+7, y+4, g_sched_stats.active_count, hi);
    scr_text(x, y+5, "CtxSw:", lb);
    scr_num(x+7, y+5, g_sched_stats.total_switches, hi);
    scr_text(x, y+6, "Up:", lb);
    scr_num(x+4, y+6, pit_get_uptime(), gr);
    scr_text(x+8, y+6, "sec", lb);
    scr_text(x, y+7, "Keys:", lb);
    scr_num(x+6, y+7, g_kb_stats.total_keypresses, hi);

    /* Mem bar */
    scr_text(x, y+9, "Mem[", lb);
    int used = 0;
    if (g_pmm_stats.total_pages > 0) {
        uint32_t u = g_pmm_stats.used_pages, t = g_pmm_stats.total_pages;
        while (t > 10000) { u >>= 1; t >>= 1; }
        if (t > 0) used = (int)(u * 16 / t);
    }
    for (int i = 0; i < 16; i++)
        scr_cell(x+4+i, y+9, i < used ? 219 : 176, i < used ? C(12, w->color) : C(8, w->color));
    scr_text(x+20, y+9, "]", lb);
}

static void render_files(wm_window_t* w) {
    int x = w->x + 1, y = w->y + 1;
    uint8_t lb = C(0, w->color);
    uint8_t fn = C(11, w->color);
    scr_text(x, y, "Files:", C(15, w->color));
    scr_num(x+7, y, g_vfs_stats.total_files, C(14, w->color));

    /* Hardcoded file names from VFS */
    const char* files[] = {"readme.txt","hello.txt","config.cfg",
                           "ai_model.dat","test.bin","hello.elf"};
    for (int i = 0; i < 6 && i < (w->h - 3); i++) {
        scr_text(x+1, y+2+i, files[i], fn);
    }
    scr_text(x, y + (w->h - 2), "Reads:", lb);
    scr_num(x+7, y + (w->h - 2), g_vfs_stats.total_reads, C(10, w->color));
}

static void render_clock(wm_window_t* w) {
    int x = w->x + 1, y = w->y + 1;
    uint8_t bg = C(0, w->color);

    uint32_t up = pit_get_uptime();
    uint32_t h = up / 3600;
    uint32_t m = (up % 3600) / 60;
    uint32_t s = up % 60;

    scr_text(x+2, y+1, "UPTIME", C(14, w->color));

    /* Big digits */
    char time_str[9];
    time_str[0] = '0' + (h / 10);
    time_str[1] = '0' + (h % 10);
    time_str[2] = ':';
    time_str[3] = '0' + (m / 10);
    time_str[4] = '0' + (m % 10);
    time_str[5] = ':';
    time_str[6] = '0' + (s / 10);
    time_str[7] = '0' + (s % 10);
    time_str[8] = '\0';
    scr_text(x+1, y+3, time_str, C(15, w->color));

    scr_text(x, y+5, "Ticks:", bg);
    scr_num(x+7, y+5, pit_get_ticks(), C(10, w->color));
}

static void render_notepad(wm_window_t* w) {
    int x = w->x + 1, y = w->y + 1;
    uint8_t bg = C(0, w->color);
    scr_text(x, y, "Type to write:", C(14, w->color));

    /* Render note content */
    int cx = 0, cy = 2;
    for (uint32_t i = 0; i < w->note_len && cy < w->h - 1; i++) {
        if (w->note[i] == '\n' || cx >= w->w - 2) { cx = 0; cy++; continue; }
        scr_cell(x + cx, y + cy, w->note[i], bg);
        cx++;
    }
    /* Cursor */
    if (cy < w->h - 1)
        scr_cell(x + cx, y + cy, '_', C(15, w->color));
}

static void render_netinfo(wm_window_t* w) {
    int x = w->x + 1, y = w->y + 1;
    uint8_t lb = C(0, w->color);
    uint8_t hi = C(15, w->color);

    scr_text(x, y, "NIC:", lb);
    scr_text(x+5, y, g_net.up ? "UP" : "DOWN", g_net.up ? C(10, w->color) : C(12, w->color));

    if (g_net.up) {
        scr_text(x, y+1, "MAC:", lb);
        char mac[18];
        const char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 6; i++) {
            mac[i*3] = hex[g_net.mac.b[i] >> 4];
            mac[i*3+1] = hex[g_net.mac.b[i] & 0xF];
            mac[i*3+2] = (i < 5) ? ':' : '\0';
        }
        mac[17] = '\0';
        scr_text(x+5, y+1, mac, hi);

        scr_text(x, y+2, "IP:", lb);
        scr_text(x+5, y+2, "10.0.2.15", C(14, w->color));
        scr_text(x, y+3, "GW:", lb);
        scr_text(x+5, y+3, "10.0.2.2", C(11, w->color));
        scr_text(x, y+5, "TX:", lb);
        scr_num(x+4, y+5, g_net.tx_packets, C(10, w->color));
        scr_text(x+8, y+5, "pkt", lb);
        scr_text(x, y+6, "RX:", lb);
        scr_num(x+4, y+6, g_net.rx_packets, C(10, w->color));
        scr_text(x+8, y+6, "pkt", lb);
        scr_text(x, y+7, "PCI:", lb);
        scr_num(x+5, y+7, g_pci.count, hi);
        scr_text(x+7, y+7, "devs", lb);
    } else {
        scr_text(x, y+2, "No NIC found", C(12, w->color));
    }
}

/* === Window Management === */

static int wm_create(const char* title, int x, int y, int w, int h,
                     uint8_t color, wm_app_t app) {
    if (g_wm.count >= WM_MAX_WINDOWS) return -1;
    /* Clamp to screen */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > WM_SCREEN_W) x = WM_SCREEN_W - w;
    if (y + h + 1 > WM_SCREEN_H - 1) y = WM_SCREEN_H - 1 - h - 1;

    wm_window_t* win = &g_wm.win[g_wm.count];
    memset(win, 0, sizeof(wm_window_t));
    strcpy(win->title, title);
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->color = color;
    win->app = app;
    win->open = true;
    g_wm.focus = g_wm.count;
    g_wm.count++;
    return g_wm.count - 1;
}

static void wm_close(int idx) {
    if (idx < 0 || idx >= g_wm.count) return;
    g_wm.win[idx].open = false;
    if (g_wm.focus == idx) {
        g_wm.focus = -1;
        /* Find another open window to focus */
        for (int i = g_wm.count - 1; i >= 0; i--)
            if (g_wm.win[i].open) { g_wm.focus = i; break; }
    }
}

static void wm_raise(int idx) {
    if (idx < 0 || idx >= g_wm.count || !g_wm.win[idx].open) return;
    g_wm.focus = idx;

    /* Move window to end of array (drawn last = on top) */
    if (idx < g_wm.count - 1) {
        wm_window_t tmp = g_wm.win[idx];
        for (int i = idx; i < g_wm.count - 1; i++)
            g_wm.win[i] = g_wm.win[i + 1];
        g_wm.win[g_wm.count - 1] = tmp;
        g_wm.focus = g_wm.count - 1;
    }
}

/* Find which window the mouse click hits (top-down) */
static int wm_hit_test(int mx, int my) {
    for (int i = g_wm.count - 1; i >= 0; i--) {
        wm_window_t* w = &g_wm.win[i];
        if (!w->open) continue;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my <= w->y + w->h)
            return i;
    }
    return -1;
}

/* === Render Everything === */

static void wm_render(void) {
    /* Desktop background */
    for (int y = 0; y < WM_SCREEN_H - 1; y++)
        for (int x = 0; x < WM_SCREEN_W; x++)
            scr_cell(x, y, 177, C(1, 0));  /* ░ dark blue pattern */

    /* Desktop title */
    scr_text(1, 0, " AIOS Window Manager ", C(15, 1));
    scr_text(24, 0, "1-6:Apps Tab:Close ESC:Exit RMB:Close", C(14, 1));

    /* Draw windows back to front */
    for (int i = 0; i < g_wm.count; i++) {
        wm_window_t* w = &g_wm.win[i];
        if (!w->open) continue;

        bool focused = (i == g_wm.focus);
        draw_window(w, focused);

        /* Render app content */
        switch (w->app) {
            case WM_APP_ABOUT:   render_about(w); break;
            case WM_APP_SYSMON:  render_sysmon(w); break;
            case WM_APP_FILES:   render_files(w); break;
            case WM_APP_CLOCK:   render_clock(w); break;
            case WM_APP_NOTEPAD: render_notepad(w); break;
            case WM_APP_NETINFO: render_netinfo(w); break;
        }
    }

    /* Taskbar */
    scr_fill(0, WM_SCREEN_H - 1, WM_SCREEN_W, 1, ' ', C(15, 3));
    scr_text(0, WM_SCREEN_H - 1, " AIOS", C(14, 3));
    int tx = 6;
    for (int i = 0; i < g_wm.count; i++) {
        if (!g_wm.win[i].open) continue;
        bool foc = (i == g_wm.focus);
        scr_cell(tx, WM_SCREEN_H - 1, '|', C(8, 3));
        tx++;
        scr_text(tx, WM_SCREEN_H - 1, g_wm.win[i].title,
                 foc ? C(15, 3) : C(0, 3));
        tx += strlen(g_wm.win[i].title) + 1;
        if (tx >= WM_SCREEN_W - 5) break;
    }

    /* Mouse cursor (inverted cell) */
    int mx = g_mouse.x / 8;
    int my = g_mouse.y / 16;
    if (mx >= WM_SCREEN_W) mx = WM_SCREEN_W - 1;
    if (my >= WM_SCREEN_H) my = WM_SCREEN_H - 1;
    uint16_t old = screen[my * WM_SCREEN_W + mx];
    uint8_t och = old & 0xFF;
    uint8_t ocol = (old >> 8) & 0xFF;
    /* Invert colors */
    uint8_t nfg = (ocol >> 4) & 0x0F;
    uint8_t nbg = ocol & 0x0F;
    screen[my * WM_SCREEN_W + mx] = (uint16_t)och | ((uint16_t)C(nfg, nbg) << 8);

    /* Blit screen to VRAM */
    memcpy(VRAM, screen, WM_SCREEN_W * WM_SCREEN_H * 2);
}

/* === Main Loop === */

void wm_run(void) {
    /* Save current screen */
    memcpy(saved_screen, VRAM, sizeof(saved_screen));

    memset(&g_wm, 0, sizeof(wm_state_t));
    g_wm.focus = -1;
    g_wm.drag = -1;
    g_wm.running = true;

    /* Open default windows */
    wm_create("About",   2,  2, 26, 9,  1, WM_APP_ABOUT);
    wm_create("SysMon", 30,  1, 24, 11, 5, WM_APP_SYSMON);
    wm_create("Clock",  58,  2, 14, 7,  4, WM_APP_CLOCK);

    keyboard_set_echo(false);

    /* Hide hardware cursor */
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);

    uint32_t last_mouse = 0;
    bool prev_lmb = false;
    bool prev_rmb = false;
    bool redraw = true;

    while (g_wm.running) {
        /* Mouse handling */
        if (g_mouse.total_moves != last_mouse ||
            g_mouse.left_btn != prev_lmb || g_mouse.right_btn != prev_rmb) {
            int mx = g_mouse.x / 8;
            int my = g_mouse.y / 16;
            if (mx >= WM_SCREEN_W) mx = WM_SCREEN_W - 1;
            if (my >= WM_SCREEN_H) my = WM_SCREEN_H - 1;

            /* LEFT click just pressed */
            if (g_mouse.left_btn && !prev_lmb) {
                int hit = wm_hit_test(mx, my);
                if (hit >= 0) {
                    /* First, raise the window we clicked */
                    wm_raise(hit);
                    wm_window_t* w = &g_wm.win[g_wm.focus];

                    /* Check if click was on [X] close button */
                    if (my == w->y && mx >= w->x + w->w - 3 && mx <= w->x + w->w - 1) {
                        wm_close(g_wm.focus);
                    }
                    /* Click on title bar? Start drag */
                    else if (my == w->y) {
                        g_wm.drag = g_wm.focus;
                        g_wm.drag_ox = mx - w->x;
                        g_wm.drag_oy = my - w->y;
                    }
                }
                redraw = true;
            }

            /* RIGHT click = close window under cursor */
            if (g_mouse.right_btn && !prev_rmb) {
                int hit = wm_hit_test(mx, my);
                if (hit >= 0) {
                    wm_close(hit);
                    redraw = true;
                }
            }

            /* Mouse dragging */
            if (g_mouse.left_btn && g_wm.drag >= 0) {
                wm_window_t* w = &g_wm.win[g_wm.drag];
                int nx = mx - g_wm.drag_ox;
                int ny = my - g_wm.drag_oy;
                /* Clamp to screen */
                if (nx < 0) nx = 0;
                if (ny < 1) ny = 1; /* Below desktop title */
                if (nx + w->w > WM_SCREEN_W) nx = WM_SCREEN_W - w->w;
                if (ny + w->h + 1 > WM_SCREEN_H - 1) ny = WM_SCREEN_H - 2 - w->h;
                if (nx != w->x || ny != w->y) {
                    w->x = nx; w->y = ny;
                    redraw = true;
                }
            }

            /* Mouse released — stop dragging */
            if (!g_mouse.left_btn && prev_lmb) {
                g_wm.drag = -1;
            }

            prev_lmb = g_mouse.left_btn;
            prev_rmb = g_mouse.right_btn;
            last_mouse = g_mouse.total_moves;
            redraw = true;
        }

        /* Keyboard */
        if (keyboard_has_char()) {
            char c = keyboard_getchar();

            if (c == 27) { /* ESC */
                g_wm.running = false;
                break;
            }

            /* Tab = close focused window */
            if (c == '\t' && g_wm.focus >= 0 && g_wm.win[g_wm.focus].open) {
                wm_close(g_wm.focus);
                redraw = true;
                continue;
            }

            /* F1-F6: Open new windows (F1=0x3B..F6=0x40 in scancode,
               but our driver gives them as special chars.
               Use number keys 1-6 instead when no notepad is focused) */
            bool notepad_focused = (g_wm.focus >= 0 &&
                                    g_wm.win[g_wm.focus].open &&
                                    g_wm.win[g_wm.focus].app == WM_APP_NOTEPAD);

            if (!notepad_focused) {
                int nx = 3 + (g_wm.count * 3) % 30;
                int ny = 2 + (g_wm.count * 2) % 10;
                switch (c) {
                    case '1': wm_create("About", nx, ny, 26, 9, 1, WM_APP_ABOUT); break;
                    case '2': wm_create("SysMon", nx, ny, 24, 11, 5, WM_APP_SYSMON); break;
                    case '3': wm_create("Files", nx, ny, 22, 10, 2, WM_APP_FILES); break;
                    case '4': wm_create("Clock", nx, ny, 14, 7, 4, WM_APP_CLOCK); break;
                    case '5': wm_create("Notepad", nx, ny, 30, 8, 6, WM_APP_NOTEPAD); break;
                    case '6': wm_create("NetInfo", nx, ny, 22, 9, 3, WM_APP_NETINFO); break;
                }
            }

            /* Type into notepad if focused */
            if (notepad_focused) {
                wm_window_t* nw = &g_wm.win[g_wm.focus];
                if (c == '\b' && nw->note_len > 0) {
                    nw->note_len--;
                } else if (c >= ' ' && c < 127 && nw->note_len < 190) {
                    nw->note[nw->note_len++] = c;
                } else if (c == '\n' && nw->note_len < 190) {
                    nw->note[nw->note_len++] = '\n';
                }
            }

            redraw = true;
        }

        /* Auto-refresh for clock/sysmon every ~500ms */
        if (pit_get_ticks() % 50 == 0) redraw = true;

        if (redraw) {
            wm_render();
            redraw = false;
        }

        __asm__ volatile("hlt");
    }

    keyboard_set_echo(true);

    /* Restore hardware cursor */
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);

    /* Restore screen */
    memcpy(VRAM, saved_screen, sizeof(saved_screen));
    vga_init();
    vga_puts("Window Manager closed.\n");
}
