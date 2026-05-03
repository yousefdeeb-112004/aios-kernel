/* =============================================================================
 * VGA Visual Dashboard — Text-Mode Graphics
 *
 * Direct writes to 0xB8000 text buffer. Each cell = char + color byte.
 * Uses colored spaces for blocks, box-drawing chars for borders.
 * No mode switching = no corruption. 100% reliable.
 * ============================================================================= */

#include <drivers/vga_gfx.h>
#include <drivers/vga.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/pit.h>
#include <drivers/ata.h>
#include <kernel/ports.h>
#include <kernel/boot_info.h>
#include <kernel/cpu.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <kernel/devtrack.h>
#include <ai/event_bus.h>
#include <lib/string.h>

#define W 80
#define H 25
#define VRAM ((uint16_t*)0xB8000)

/* Save/restore text screen */
static uint16_t saved_screen[W * H];

/* Colors */
#define C(fg, bg) ((uint8_t)((fg) | ((bg) << 4)))

/* Direct cell write */
static void cell(int x, int y, char ch, uint8_t color) {
    if (x >= 0 && x < W && y >= 0 && y < H)
        VRAM[y * W + x] = (uint16_t)((uint8_t)ch) | ((uint16_t)color << 8);
}

/* Colored block (space with background) */
static void block(int x, int y, uint8_t bg) {
    cell(x, y, ' ', C(0, bg));
}

/* Fill rectangle with colored blocks */
static void fill(int x, int y, int w, int h, uint8_t bg) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            block(x + i, y + j, bg);
}

/* Draw text at position */
static void text(int x, int y, const char* str, uint8_t fg, uint8_t bg) {
    uint8_t color = C(fg, bg);
    while (*str && x < W) {
        cell(x++, y, *str++, color);
    }
}

/* Draw number at position */
static void num(int x, int y, uint32_t val, uint8_t fg, uint8_t bg) {
    char buf[12]; int i = 0;
    if (val == 0) { buf[i++] = '0'; }
    else { while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; } }
    char rev[12]; for (int j = 0; j < i; j++) rev[j] = buf[i-1-j]; rev[i] = 0;
    text(x, y, rev, fg, bg);
}

/* Horizontal line */
static void hline(int x, int y, int w, uint8_t fg, uint8_t bg) {
    uint8_t color = C(fg, bg);
    for (int i = 0; i < w; i++)
        cell(x + i, y, 196, color);  /* '─' */
}

/* Draw bordered box */
static void box(int x, int y, int w, int h, uint8_t fg, uint8_t bg, const char* title) {
    uint8_t bc = C(fg, bg);
    uint8_t fc = C(0, bg);

    /* Fill interior */
    fill(x, y, w, h, bg);

    /* Top/bottom borders */
    cell(x, y, 218, bc);           /* ┌ */
    cell(x + w - 1, y, 191, bc);   /* ┐ */
    cell(x, y + h - 1, 192, bc);   /* └ */
    cell(x + w - 1, y + h - 1, 217, bc); /* ┘ */
    for (int i = 1; i < w - 1; i++) {
        cell(x + i, y, 196, bc);         /* ─ top */
        cell(x + i, y + h - 1, 196, bc); /* ─ bottom */
    }
    for (int j = 1; j < h - 1; j++) {
        cell(x, y + j, 179, bc);         /* │ left */
        cell(x + w - 1, y + j, 179, bc); /* │ right */
    }

    /* Title */
    if (title && title[0]) {
        int tlen = strlen(title);
        int tx = x + (w - tlen - 2) / 2;
        cell(tx, y, 180, bc);  /* ┤ */
        text(tx + 1, y, title, 15, bg);
        cell(tx + tlen + 1, y, 195, bc); /* ├ */
    }
}

/* Progress bar (horizontal) */
static void progress(int x, int y, int w, uint32_t val, uint32_t max_val, uint8_t fg, uint8_t bg) {
    int filled = 0;
    if (max_val > 0) {
        uint32_t sv = val, sm = max_val;
        while (sm > 10000) { sv >>= 1; sm >>= 1; }
        if (sm > 0) filled = (int)(sv * (uint32_t)(w) / sm);
    }
    if (filled > w) filled = w;
    if (filled < 0) filled = 0;
    for (int i = 0; i < w; i++) {
        if (i < filled)
            cell(x + i, y, 219, C(fg, 0));  /* █ filled */
        else
            cell(x + i, y, 176, C(8, 0));   /* ░ empty */
    }
}

/* Vertical bar (grows upward) */
static void vbar(int x, int y_bottom, int max_h, uint32_t val, uint32_t max_val, uint8_t color) {
    int filled = 0;
    if (max_val > 0) {
        uint32_t sv = val, sm = max_val;
        while (sm > 10000) { sv >>= 1; sm >>= 1; }
        if (sm > 0) filled = (int)(sv * (uint32_t)max_h / sm);
    }
    if (filled > max_h) filled = max_h;
    for (int j = 0; j < max_h; j++) {
        if (j < filled)
            cell(x, y_bottom - j, 219, C(color, 0));     /* █ */
        else
            cell(x, y_bottom - j, 176, C(8, 0));         /* ░ */
    }
}

/* =========================================================================
 * Dashboard Pages
 * ========================================================================= */

static void draw_page1(void) {
    /* Page 1: System Overview */
    fill(0, 0, W, H, 0);  /* Black background */

    /* Title bar */
    fill(0, 0, W, 1, 1);  /* Blue bar */
    text(1, 0, " AIOS KERNEL v1.3 - System Dashboard ", 15, 1);
    text(55, 0, "[1]Sys [2]Mem [3]IO [4]AI", 14, 1);

    /* System Info box */
    box(1, 2, 38, 8, 3, 0, " System Info ");
    text(3, 3, "Kernel:", 7, 0);  text(12, 3, "AIOS v1.3.0", 10, 0);
    text(3, 4, "CPU:", 7, 0);     text(12, 4, g_cpu_info.vendor, 11, 0);
    text(3, 5, "RAM:", 7, 0);     num(12, 5, g_boot_info.total_memory_kb/1024, 14, 0);
    text(16, 5, "MB total", 7, 0);
    text(3, 6, "Uptime:", 7, 0);  num(12, 6, pit_get_uptime(), 13, 0);
    text(18, 6, "sec", 7, 0);
    text(3, 7, "Ticks:", 7, 0);   num(12, 7, pit_get_ticks(), 13, 0);
    text(3, 8, "Keys:", 7, 0);    num(12, 8, g_kb_stats.total_keypresses, 11, 0);

    /* Process box */
    box(41, 2, 38, 8, 5, 0, " Processes ");
    text(43, 3, "Active:", 7, 0);   num(52, 3, g_sched_stats.active_count, 10, 0);
    text(43, 4, "Created:", 7, 0);  num(52, 4, g_sched_stats.total_created, 14, 0);
    text(43, 5, "Ended:", 7, 0);    num(52, 5, g_sched_stats.total_terminated, 12, 0);
    text(43, 6, "Ctx Sw:", 7, 0);   num(52, 6, g_sched_stats.total_switches, 13, 0);
    text(43, 7, "Syscalls:", 7, 0); num(52, 7, g_syscall_stats.total, 11, 0);
    text(43, 8, "Files:", 7, 0);    num(52, 8, g_vfs_stats.total_files, 14, 0);

    /* Disk box */
    box(1, 11, 38, 5, 9, 0, " Disk ");
    if (g_ata_disk.present) {
        text(3, 12, "Model:", 7, 0); text(10, 12, g_ata_disk.model, 11, 0);
        text(3, 13, "Size:", 7, 0);  num(10, 13, g_ata_disk.size_mb, 14, 0);
        text(16, 13, "MB", 7, 0);
        text(20, 13, "R:", 7, 0); num(22, 13, g_ata_disk.total_reads, 10, 0);
        text(27, 13, "W:", 7, 0); num(29, 13, g_ata_disk.total_writes, 10, 0);
        text(3, 14, "Errors:", 7, 0); num(11, 14, g_ata_disk.total_errors, 12, 0);
    } else {
        text(3, 12, "No disk attached", 12, 0);
        text(3, 13, "Use: -hda disk.img", 8, 0);
    }

    /* Mouse box */
    box(41, 11, 38, 5, 13, 0, " Mouse ");
    text(43, 12, "Pos:", 7, 0);
    num(48, 12, g_mouse.x, 14, 0); text(54, 12, ",", 7, 0);
    num(55, 12, g_mouse.y, 14, 0);
    text(43, 13, "Moves:", 7, 0);  num(50, 13, g_mouse.total_moves, 11, 0);
    text(58, 13, "Clicks:", 7, 0); num(66, 13, g_mouse.total_clicks, 11, 0);
    text(43, 14, "Buttons:", 7, 0);
    text(52, 14, g_mouse.left_btn ? "L" : ".", g_mouse.left_btn ? 10 : 8, 0);
    text(53, 14, g_mouse.middle_btn ? "M" : ".", g_mouse.middle_btn ? 10 : 8, 0);
    text(54, 14, g_mouse.right_btn ? "R" : ".", g_mouse.right_btn ? 10 : 8, 0);

    /* Color palette */
    box(1, 17, 78, 3, 7, 0, " Color Palette ");
    for (int i = 0; i < 16; i++) {
        block(3 + i * 4, 18, (uint8_t)i);
        block(4 + i * 4, 18, (uint8_t)i);
        block(5 + i * 4, 18, (uint8_t)i);
    }

    /* Status bar */
    fill(0, 21, W, 1, 1);
    text(1, 21, "Mouse active | LMB=draw on canvas | Keys: 1-4=pages ESC=exit", 14, 1);

    /* Drawing canvas area */
    box(1, 22, 78, 3, 8, 0, " Canvas - Click to Draw ");
}

static void draw_page2(void) {
    /* Page 2: Memory Details */
    fill(0, 0, W, H, 0);
    fill(0, 0, W, 1, 2);
    text(1, 0, " AIOS - Memory Dashboard ", 15, 2);
    text(55, 0, "[1]Sys [2]Mem [3]IO [4]AI", 14, 2);

    /* PMM Stats */
    box(1, 2, 38, 9, 10, 0, " Physical Memory ");
    text(3, 3, "Total Pages:", 7, 0); num(17, 3, g_pmm_stats.total_pages, 15, 0);
    text(3, 4, "Used Pages:", 7, 0);  num(17, 4, g_pmm_stats.used_pages, 12, 0);
    text(3, 5, "Free Pages:", 7, 0);  num(17, 5, g_pmm_stats.free_pages, 10, 0);
    text(3, 6, "Peak Used:", 7, 0);   num(17, 6, g_pmm_stats.peak_used, 14, 0);
    text(3, 7, "Allocs:", 7, 0);      num(17, 7, g_pmm_stats.alloc_count, 11, 0);
    text(3, 8, "Frees:", 7, 0);       num(17, 8, g_pmm_stats.free_count, 11, 0);
    /* Usage bar */
    text(3, 9, "Usage:", 7, 0);
    progress(10, 9, 27, g_pmm_stats.used_pages, g_pmm_stats.total_pages, 12, 0);

    /* Heap Stats */
    box(41, 2, 38, 9, 14, 0, " Kernel Heap ");
    text(43, 3, "Heap Size:", 7, 0);  num(55, 3, g_heap_stats.heap_size/1024, 15, 0);
    text(63, 3, "KB", 7, 0);
    text(43, 4, "In Use:", 7, 0);     num(55, 4, g_heap_stats.total_allocated, 12, 0);
    text(65, 4, "B", 7, 0);
    text(43, 5, "Allocs:", 7, 0);     num(55, 5, g_heap_stats.alloc_count, 10, 0);
    text(43, 6, "Frees:", 7, 0);      num(55, 6, g_heap_stats.free_count, 10, 0);
    text(43, 7, "Usage:", 7, 0);
    progress(50, 7, 27, g_heap_stats.total_allocated, g_heap_stats.heap_size, 14, 0);

    /* VMM Stats */
    box(1, 12, 78, 5, 11, 0, " Virtual Memory ");
    text(3, 13, "Pages Mapped:", 7, 0);  num(18, 13, g_vmm_stats.pages_mapped, 10, 0);
    text(3, 14, "Page Faults:", 7, 0);   num(18, 14, g_vmm_stats.page_faults, 12, 0);
    text(3, 15, "Maps Created:", 7, 0);  num(18, 15, g_vmm_stats.maps_created, 14, 0);
    text(40, 13, "Identity mapped: 0-16MB", 8, 0);
    text(40, 14, "Heap starts at: 0x800000", 8, 0);

    /* Memory map visualization */
    box(1, 18, 78, 6, 3, 0, " Memory Map (each block = ~4MB) ");
    /* 0-1MB: BIOS/Kernel */
    fill(3, 19, 2, 1, 4); text(3, 20, "BIOS", 4, 0);
    fill(5, 19, 2, 1, 12); text(5, 20, "Kern", 12, 0);
    /* 1-8MB: Free */
    fill(7, 19, 14, 1, 2); text(7, 20, "Free Memory", 2, 0);
    /* 8MB+: Heap */
    fill(21, 19, 4, 1, 14); text(21, 20, "Heap", 14, 0);
    /* Rest: Free */
    fill(25, 19, 50, 1, 2);
    text(35, 20, "Available", 2, 0);

    fill(0, H-1, W, 1, 2);
    text(1, H-1, "ESC=exit | 1-4=pages", 14, 2);
}

static void draw_page3(void) {
    /* Page 3: I/O Details */
    fill(0, 0, W, H, 0);
    fill(0, 0, W, 1, 5);
    text(1, 0, " AIOS - I/O Dashboard ", 15, 5);
    text(55, 0, "[1]Sys [2]Mem [3]IO [4]AI", 14, 5);

    /* VFS Files */
    box(1, 2, 78, 12, 11, 0, " Virtual File System ");
    text(3, 3, "File                Size    Opens  Reads", 15, 0);
    hline(3, 4, 70, 8, 0);
    /* List files manually from VFS */
    text(3, 5, "readme.txt", 11, 0);  text(24, 5, "119B", 14, 0);
    text(3, 6, "hello.txt", 11, 0);   text(24, 6, "33B", 14, 0);
    text(3, 7, "config.cfg", 11, 0);  text(24, 7, "87B", 14, 0);
    text(3, 8, "ai_model.dat", 11, 0);text(24, 8, "48B", 14, 0);
    text(3, 9, "test.bin", 11, 0);    text(24, 9, "64B", 14, 0);
    text(3, 10, "hello.elf", 13, 0);  text(24, 10, "6028B", 14, 0);
    text(3, 12, "Total files:", 7, 0); num(17, 12, g_vfs_stats.total_files, 15, 0);
    text(25, 12, "Total reads:", 7, 0); num(39, 12, g_vfs_stats.total_reads, 15, 0);

    /* Keyboard + Timer */
    box(1, 15, 38, 6, 14, 0, " Input Devices ");
    text(3, 16, "KB Presses:", 7, 0);  num(16, 16, g_kb_stats.total_keypresses, 10, 0);
    text(3, 17, "KB Releases:", 7, 0); num(16, 17, g_kb_stats.total_releases, 10, 0);
    text(3, 18, "Timer IRQs:", 7, 0);  num(16, 18, g_timer_stats.irq_count, 13, 0);
    text(3, 19, "Timer Hz:", 7, 0);    num(16, 19, PIT_TARGET, 14, 0);

    /* Syscalls */
    box(41, 15, 38, 6, 12, 0, " Syscalls ");
    text(43, 16, "write:", 7, 0);   num(51, 16, g_syscall_stats.counts[1], 10, 0);
    text(57, 16, "read:", 7, 0);    num(63, 16, g_syscall_stats.counts[2], 10, 0);
    text(43, 17, "getpid:", 7, 0);  num(51, 17, g_syscall_stats.counts[3], 10, 0);
    text(57, 17, "yield:", 7, 0);   num(63, 17, g_syscall_stats.counts[5], 10, 0);
    text(43, 18, "uptime:", 7, 0);  num(51, 18, g_syscall_stats.counts[6], 10, 0);
    text(57, 18, "stats:", 7, 0);   num(63, 18, g_syscall_stats.counts[10], 10, 0);
    text(43, 19, "Total:", 7, 0);   num(51, 19, g_syscall_stats.total, 15, 0);

    fill(0, H-1, W, 1, 5);
    text(1, H-1, "ESC=exit | 1-4=pages", 14, 5);
}

static void draw_page4(void) {
    /* Page 4: AI Dashboard */
    fill(0, 0, W, H, 0);
    fill(0, 0, W, 1, 4);
    text(1, 0, " AIOS - AI Intelligence Dashboard ", 15, 4);
    text(55, 0, "[1]Sys [2]Mem [3]IO [4]AI", 14, 4);

    /* Event Bus */
    box(1, 2, 38, 8, 11, 0, " AI Event Bus ");
    text(3, 3, "Total Events:", 7, 0);  num(18, 3, g_ai_event_stats.total_events, 10, 0);
    text(3, 4, "Subscribers:", 7, 0);   num(18, 4, g_ai_event_stats.total_subscribers, 14, 0);
    text(3, 5, "Dropped:", 7, 0);       num(18, 5, g_ai_event_stats.dropped_events, 12, 0);
    text(3, 7, "Event Types Active:", 7, 0);
    int active = 0;
    for (int i = 0; i < (int)AI_EVT_MAX; i++)
        if (g_ai_event_stats.counts[i] > 0) active++;
    num(24, 7, active, 13, 0);
    text(27, 7, "/", 7, 0); num(28, 7, AI_EVT_MAX, 7, 0);
    text(3, 8, "Coverage:", 7, 0);
    progress(13, 8, 24, active, AI_EVT_MAX, 11, 0);

    /* Perf Counters */
    box(41, 2, 38, 8, 13, 0, " Performance ");
    ai_perf_sample_t* s = ai_get_latest_sample();
    if (s) {
        text(43, 3, "CPU Idle:", 7, 0); num(54, 3, s->cpu_idle_pct, 10, 0);
        text(58, 3, "%", 7, 0);
        text(43, 4, "Sys/sec:", 7, 0);  num(54, 4, s->syscalls_per_sec, 14, 0);
        text(43, 5, "SW/sec:", 7, 0);   num(54, 5, s->ctx_switches_per_sec, 14, 0);
        text(43, 6, "IRQ/sec:", 7, 0);  num(54, 6, s->interrupts_per_sec, 14, 0);
        text(43, 7, "Key/sec:", 7, 0);  num(54, 7, s->kb_presses_per_sec, 14, 0);
    } else {
        text(43, 4, "No samples yet", 8, 0);
    }
    text(43, 8, "Samples:", 7, 0); num(53, 8, g_ai_perf.count, 13, 0);
    text(57, 8, "/60", 7, 0);

    /* Developer Tracking */
    devtrack_update();
    box(1, 11, 78, 6, 14, 0, " Developer Tracking ");
    text(3, 12, "Activity:", 7, 0);
    const char* act = devtrack_activity_str();
    uint8_t act_color = 10;
    if (g_devtrack.current_activity == DEV_IDLE) act_color = 8;
    else if (g_devtrack.current_activity == DEV_COMPILING) act_color = 13;
    text(14, 12, act, act_color, 0);
    uint32_t ss = (pit_get_ticks() - g_devtrack.session_start_tick) / PIT_TARGET;
    text(28, 12, "Session:", 7, 0); num(38, 12, ss/60, 14, 0);
    text(42, 12, "m", 7, 0); num(43, 12, ss%60, 14, 0); text(46, 12, "s", 7, 0);
    text(3, 13, "Keystrokes:", 7, 0);  num(16, 13, g_devtrack.total_keystrokes, 10, 0);
    text(25, 13, "Mouse:", 7, 0);      num(33, 13, g_devtrack.total_mouse_events, 10, 0);
    text(42, 13, "Bursts:", 7, 0);     num(51, 13, g_devtrack.typing_bursts, 14, 0);
    text(3, 14, "Files Opened:", 7, 0); num(18, 14, g_devtrack.files_opened, 11, 0);
    text(25, 14, "Compiles:", 7, 0);    num(36, 14, g_devtrack.compile.total_compilations, 13, 0);
    text(3, 15, "Idle:", 7, 0); num(10, 15, g_devtrack.idle_seconds, 8, 0); text(16, 15, "sec", 8, 0);
    if (g_devtrack.break_suggested)
        text(25, 15, "*** AI: Take a break! ***", 14, 0);

    /* Activity bar chart */
    box(1, 18, 78, 6, 3, 0, " System Activity Bars ");
    text(3, 19, "Mem", 12, 0);
    progress(7, 19, 20, g_pmm_stats.used_pages, g_pmm_stats.total_pages, 12, 0);
    text(3, 20, "Heap", 14, 0);
    progress(7, 20, 20, g_heap_stats.total_allocated, g_heap_stats.heap_size, 14, 0);
    text(30, 19, "IO", 11, 0);
    progress(34, 19, 20, g_vfs_stats.total_reads, 50, 11, 0);
    text(30, 20, "Sys", 13, 0);
    progress(34, 20, 20, g_syscall_stats.total, 50, 13, 0);
    text(57, 19, "Keys", 10, 0);
    progress(62, 19, 14, g_kb_stats.total_keypresses, 200, 10, 0);
    text(57, 20, "Ctx", 5, 0);
    progress(62, 20, 14, g_sched_stats.total_switches, 50, 5, 0);

    fill(0, H-1, W, 1, 4);
    text(1, H-1, "ESC=exit | 1-4=pages", 14, 4);
}

/* =========================================================================
 * Main Dashboard Loop
 * ========================================================================= */

void gfx_demo(void) {
    /* Save current text screen */
    memcpy(saved_screen, VRAM, W * H * 2);

    keyboard_set_echo(false);

    int page = 1;
    bool redraw = true;
    uint32_t last_mouse = g_mouse.total_moves;

    /* Hide hardware cursor */
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);

    while (1) {
        if (redraw) {
            switch (page) {
                case 1: draw_page1(); break;
                case 2: draw_page2(); break;
                case 3: draw_page3(); break;
                case 4: draw_page4(); break;
            }
            redraw = false;
        }

        /* Update mouse position on screen */
        if (g_mouse.total_moves != last_mouse) {
            int mx = g_mouse.x / 8;
            int my = g_mouse.y / 16;
            if (mx >= W) mx = W - 1;
            if (my >= H) my = H - 1;

            /* Draw cursor indicator in title bar */
            text(40, 0, "   ", 7, page == 1 ? 1 : page == 2 ? 2 : page == 3 ? 5 : 4);
            if (g_mouse.left_btn)
                text(40, 0, "LMB", 0, 10);
            else if (g_mouse.right_btn)
                text(40, 0, "RMB", 0, 12);

            /* On page 1 canvas area: allow drawing */
            if (page == 1 && my >= 22 && my <= 23 && mx >= 2 && mx <= 77) {
                if (g_mouse.left_btn)
                    block(mx, my, 15);  /* White */
                else if (g_mouse.right_btn)
                    block(mx, my, 0);   /* Black (erase) */
            }

            last_mouse = g_mouse.total_moves;
            devtrack_on_mouse_move();
        }

        /* Check keyboard */
        if (keyboard_has_char()) {
            char c = keyboard_getchar();
            devtrack_on_keypress();

            if (c == 27) break;  /* ESC = exit */
            if (c == '1') { page = 1; redraw = true; }
            if (c == '2') { page = 2; redraw = true; }
            if (c == '3') { page = 3; redraw = true; }
            if (c == '4') { page = 4; redraw = true; }
            /* Auto-refresh current page on any other key */
            if (c != '1' && c != '2' && c != '3' && c != '4' && c != 27)
                redraw = true;
        }

        __asm__ volatile("hlt");
    }

    keyboard_set_echo(true);

    /* Restore hardware cursor */
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);

    /* Restore previous text screen */
    memcpy(VRAM, saved_screen, W * H * 2);

    /* Restore VGA cursor position */
    vga_init();
    vga_puts("Dashboard closed. Back in shell.\n");
}
