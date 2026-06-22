/* =============================================================================
 * AIOS Shell — Arabic-Inspired Command Interface (Tier 1)
 *
 * Commands:
 *   db   (عرض display)  — list files
 *   rq   (اقرأ read)    — read file: rq filename
 *   nz   (نظر look)     — system info
 *   am   (عمليات procs)  — show processes
 *   zk   (ذاكرة memory) — memory stats
 *   dk   (ذكاء intel)   — AI report
 *   ms   (مسح clear)    — clear screen
 *   sd   (ساعد help)    — show help
 *   wqf  (وقف halt)     — halt system
 *   ktb  (كتب write)    — create file: ktb filename content
 *
 * Features: command history (up/down arrows), color themes, mouse cursor
 * ============================================================================= */

#include <kernel/shell.h>
#include <kernel/ports.h>
#include <kernel/vfs.h>
#include <kernel/editor.h>
#include <kernel/process.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/heap.h>
#include <kernel/syscall.h>
#include <kernel/cpu.h>
#include <kernel/boot_info.h>
#include <kernel/devtrack.h>
#include <kernel/usermode.h>
#include <kernel/elf.h>
#include <kernel/aios_fs.h>
#include <kernel/lock.h>
#include <kernel/panic.h>
#include <kernel/dev.h>
#include <kernel/pipe.h>
#include <drivers/ata.h>
#include <drivers/vga_gfx.h>
#include <ai/event_bus.h>
#include <ai/agent.h>
#include <kernel/pci.h>
#include <kernel/net.h>
#include <kernel/wm.h>
#include <kernel/smp.h>
#include <kernel/smp.h>
#include <drivers/vga.h>
#include <drivers/rtc.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/pit.h>
#include <drivers/serial.h>
#include <lib/string.h>
#include <lib/kprintf.h>

/* === Input buffer === */
static char cmd_buf[SHELL_CMD_MAX];
static uint32_t cmd_len = 0;

/* === Command history === */
static char history[SHELL_HISTORY][SHELL_CMD_MAX];
static uint32_t hist_count = 0;
static uint32_t hist_write = 0;
static int32_t hist_browse = -1;  /* -1 = not browsing */

/* === Color theme === */
static vga_color_t theme_prompt = VGA_LIGHT_GREEN;
static vga_color_t theme_output = VGA_LIGHT_GREY;
static vga_color_t theme_header = VGA_LIGHT_CYAN;
static vga_color_t theme_error  = VGA_LIGHT_RED;
static vga_color_t theme_value  = VGA_WHITE;

/* === Mouse cursor state === */
static int32_t mouse_prev_x = -1;
static int32_t mouse_prev_y = -1;
static uint16_t mouse_saved_char = 0;

/* Forward declarations */
static void shell_exec(const char* line);
static void shell_prompt(void);
static void shell_readline(void);
static void history_add(const char* line);
static void history_recall(int32_t idx);
static void mouse_cursor_update(void);
static void mouse_cursor_hide(void);

/* === Skip whitespace === */
static const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* === Prompt === */
static void shell_prompt(void) {
    vga_puts_color("aios", theme_prompt, VGA_BLACK);
    vga_puts_color("> ", VGA_YELLOW, VGA_BLACK);
    vga_set_color(theme_output, VGA_BLACK);
}

/* === Mouse cursor drawing === */
static void mouse_cursor_update(void) {
    uint16_t* vga = (uint16_t*)VGA_BUFFER;
    int32_t cx = g_mouse.x / 8;   /* Convert pixel to cell */
    int32_t cy = g_mouse.y / 16;

    if (cx < 0) cx = 0;
    if (cx >= VGA_WIDTH) cx = VGA_WIDTH - 1;
    if (cy < 0) cy = 0;
    if (cy >= VGA_HEIGHT) cy = VGA_HEIGHT - 1;

    /* Same position? Nothing to do */
    if (cx == mouse_prev_x && cy == mouse_prev_y) return;

    /* Restore previous position */
    if (mouse_prev_x >= 0 && mouse_prev_y >= 0) {
        vga[mouse_prev_y * VGA_WIDTH + mouse_prev_x] = mouse_saved_char;
    }

    /* Save new position's character */
    mouse_saved_char = vga[cy * VGA_WIDTH + cx];

    /* Draw cursor: invert colors */
    uint8_t ch = (uint8_t)(mouse_saved_char & 0xFF);
    uint8_t orig_color = (uint8_t)(mouse_saved_char >> 8);
    uint8_t fg = (orig_color >> 4) & 0x0F;  /* Swap fg/bg */
    uint8_t bg = orig_color & 0x0F;
    if (fg == bg) fg = VGA_WHITE;  /* Make sure visible */
    vga[cy * VGA_WIDTH + cx] = (uint16_t)ch | ((uint16_t)((fg | (bg << 4))) << 8);

    mouse_prev_x = cx;
    mouse_prev_y = cy;
}

static void mouse_cursor_hide(void) {
    if (mouse_prev_x >= 0 && mouse_prev_y >= 0) {
        uint16_t* vga = (uint16_t*)VGA_BUFFER;
        vga[mouse_prev_y * VGA_WIDTH + mouse_prev_x] = mouse_saved_char;
        mouse_prev_x = -1;
        mouse_prev_y = -1;
    }
}

/* === Command History === */
static void history_add(const char* line) {
    if (!line[0]) return;  /* Don't store empty */
    strcpy(history[hist_write], line);
    hist_write = (hist_write + 1) % SHELL_HISTORY;
    if (hist_count < SHELL_HISTORY) hist_count++;
    hist_browse = -1;
}

static void history_recall(int32_t idx) {
    if (idx < 0 || (uint32_t)idx >= hist_count) return;
    /* Erase current input from screen */
    while (cmd_len > 0) { vga_putchar('\b'); cmd_len--; }
    /* Calculate actual index in ring buffer */
    int32_t actual = (int32_t)hist_write - 1 - idx;
    if (actual < 0) actual += SHELL_HISTORY;
    strcpy(cmd_buf, history[actual]);
    cmd_len = strlen(cmd_buf);
    vga_puts(cmd_buf);
}

/* === Read a line with editing + arrow key history === */
static void shell_readline(void) {
    cmd_len = 0;
    cmd_buf[0] = '\0';
    hist_browse = -1;

    keyboard_set_echo(false);  /* Shell handles its own echo */

    for (;;) {
        /* Update mouse cursor while waiting */
        if (g_mouse.total_moves > 0) {
            mouse_cursor_update();
        }

        if (!keyboard_has_char()) {
            /* Poll network while idle — handles echo server + ARP */
            net_poll();
            /* Yield to other processes instead of blocking with hlt.
             * This lets background processes run + print output
             * while the shell waits for keyboard input. */
            proc_yield();
            continue;
        }

        char c = keyboard_getchar();
        devtrack_on_keypress();

        if (c == '\n') {
            /* If scrolled back, Enter returns to live view */
            if (vga_is_scrolled()) {
                vga_scroll_reset();
                continue;
            }
            mouse_cursor_hide();
            vga_putchar('\n');
            cmd_buf[cmd_len] = '\0';
            keyboard_set_echo(true);
            return;
        }

        if (c == '\b') {
            if (vga_is_scrolled()) { vga_scroll_reset(); continue; }
            if (cmd_len > 0) {
                cmd_len--;
                vga_putchar('\b');
            }
            continue;
        }

        /* Arrow UP or Ctrl+P = recall older command */
        if (c == KEY_UP || c == 16) {
            if (vga_is_scrolled()) { vga_scroll_reset(); continue; }
            if (hist_count > 0) {
                if (hist_browse < (int32_t)hist_count - 1)
                    hist_browse++;
                history_recall(hist_browse);
            }
            continue;
        }

        /* Arrow DOWN or Ctrl+N = recall newer command */
        if (c == KEY_DOWN || c == 14) {
            if (vga_is_scrolled()) { vga_scroll_reset(); continue; }
            if (hist_browse > 0) {
                hist_browse--;
                history_recall(hist_browse);
            } else if (hist_browse == 0) {
                /* Back to empty line */
                hist_browse = -1;
                while (cmd_len > 0) { vga_putchar('\b'); cmd_len--; }
                cmd_buf[0] = '\0';
            }
            continue;
        }

        /* ESC key (27) — always return to live if scrolled */
        if (c == 27) {
            if (vga_is_scrolled()) vga_scroll_reset();
            continue;
        }

        /* Shift+Up (KEY_PGUP) — scroll back through output history */
        if (c == KEY_PGUP) {
            vga_scroll_back();
            continue;
        }

        /* Shift+Down (KEY_PGDN) — jump straight back to live view */
        if (c == KEY_PGDN) {
            if (vga_is_scrolled()) vga_scroll_reset();
            continue;
        }

        /* Any other key while scrolled — return to live first */
        if (vga_is_scrolled()) {
            vga_scroll_reset();
            /* Don't consume the key — let it fall through to be typed */
        }

        /* Tab = ignore */
        if (c == '\t') continue;

        /* Regular printable character */
        if (c >= ' ' && c < 127 && cmd_len < SHELL_CMD_MAX - 1) {
            cmd_buf[cmd_len++] = c;
            vga_putchar(c);
        }
    }
}

/* === COMMANDS === */

static void cmd_help(void) {
    vga_puts_color("=== AIOS Shell - 61 Commands ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_color("  [Files]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" db rq ktb thrr hdhf smm mlf nskhh mjld dkhl pwd\n");
    vga_puts_color("  [System]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" nz am zk dk ms sd wqf aqtl khlf fzh ishara thkra wqt\n");
    vga_puts_color("  [Devices]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" ajhz dev\n");
    vga_puts_color("  [IPC]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" anbb\n");
    vga_puts_color("  [Kernel]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" shgl jrb mft\n");
    vga_puts_color("  [Disk]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" qrs qra hfz tnsyq hfzk hmml dskfs\n");
    vga_puts_color("  [AI]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" dka tnb thll rsd\n");
    vga_puts_color("  [Network]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" shbk pci irsl stlm sma dhcp tcp dns\n");
    vga_puts_color("  [Process]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" intdhr  |  Shell pipes: cmd1 | cmd2\n");
    vga_puts_color("  [Desktop]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" rsm nfdh\n");
    vga_puts_color("  [SMP]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" nwat qfl mwzy\n");
    vga_puts_color("  [Keys]", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" Up/Down=History  Shift+Up/Down=Scroll  Ctrl+S=Save\n");
    vga_puts("\n");
    vga_puts_color("  File cmds:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   db          List files         rq <f>   View file\n");
    vga_puts("   ktb <f> <t> Create/append      thrr <f> Edit in editor\n");
    vga_puts("   hdhf <f>    Delete file        smm <o> <n> Rename\n");
    vga_puts("   mlf <f>     File info          nskhh <s> <d> Copy\n");
    vga_puts_color("   mjld <d>    Create directory    dkhl <d>  Change dir\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   pwd         Print working dir\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("  System:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   nz  System info   am  Processes   zk  Memory\n");
    vga_puts("   ms  Clear screen  wqf Halt   wqt  Show date/time\n");
    vga_puts_color("   aqtl <pid> [sig]  Send signal (default: SIGTERM)\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   ishara  Signal info  ishara demo  Signal demo\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   thkra   sbrk heap allocation demo\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   khlf  Launch background task (non-blocking demo)\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   fzh <1|2|3>  Panic test (div0/pgfault/manual)\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("  Kernel:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   shgl <f> Run ELF (Ring 3, isolated)   jrb Preempt test\n");
    vga_puts_color("  Disk (persistent):\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   qrs  Disk info    qra <n> Read sect  hfz <n> <t> Write\n");
    vga_puts_color("   tnsyq Format disk  hfzk Sync VFS->disk\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   hmml  Load disk->VFS  dskfs Filesystem info\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("  AI:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   dka AI status  tnb Predict  thll Analysis  rsd Monitor\n");
    vga_puts_color("  Net:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   shbk Status  pci Devices  irsl <t> Send  stlm Receive\n");
    vga_puts_color("   sma  Echo server (start/stop)  dhcp  Get IP via DHCP\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   tcp  TCP connections (connect/send/recv/close/list)\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   dns <host>  Resolve hostname to IP\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("   intdhr  Waitpid demo  |  Pipe: cmd1 | cmd2\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("  Desktop:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   rsm Dashboard    nfdh Window Manager\n");
    vga_puts_color("  SMP:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   nwat CPU cores   qfl Lock test   mwzy Parallel work\n");
    vga_puts_color("  Devices:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   ajhz List devices  dev r <n> Read  dev w <n> <t> Write\n");
    vga_puts_color("  IPC/Pipes:\n", VGA_WHITE, VGA_BLACK);
    vga_puts("   anbb  List pipes       anbb mk/rm <n>  Create/destroy\n");
    vga_puts("   anbb w <n> <t> Write   anbb r <n> Read  anbb demo\n");
}

static void cmd_db(void) {
    vga_puts_color("=== Files (db) ===\n", theme_header, VGA_BLACK);
    vfs_list();
}

static void cmd_rq(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: rq <filename>\n", theme_error, VGA_BLACK);
        return;
    }
    int32_t fd = vfs_open(arg);
    if (fd < 0) {
        vga_puts_color("File not found: ", theme_error, VGA_BLACK);
        vga_puts(arg);
        vga_puts("\n");
        return;
    }
    devtrack_on_file_open(arg);
    vga_puts_color("--- ", theme_header, VGA_BLACK);
    vga_puts_color(arg, theme_value, VGA_BLACK);
    vga_puts_color(" ---\n", theme_header, VGA_BLACK);
    char buf[512];
    int32_t n;
    while ((n = vfs_read(fd, buf, 511)) > 0) {
        buf[n] = '\0';
        vga_puts(buf);
    }
    vfs_close(fd);
    /* Ensure newline at end */
    vga_puts("\n");
}

/* === Feature #11: Auto-sync to disk after file changes === */
static void auto_sync(void) {
    if (g_ata_disk.present && aiosfs_detect()) {
        aiosfs_sync();
    }
}

static void cmd_ktb(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: ktb <filename.txt> <content>\n", theme_error, VGA_BLACK);
        return;
    }
    char fname[VFS_MAX_NAME];
    uint32_t fi = 0;
    while (arg[fi] && arg[fi] != ' ' && fi < VFS_MAX_NAME - 1) {
        fname[fi] = arg[fi]; fi++;
    }
    fname[fi] = '\0';
    const char* content = skip_spaces(arg + fi);

    if (vfs_exists(fname)) {
        /* Append to existing file */
        int32_t fd = vfs_open_mode(fname, VFS_MODE_WRITE | VFS_MODE_APPEND);
        if (fd < 0) {
            vga_puts_color("Cannot write (read-only?).\n", theme_error, VGA_BLACK);
            return;
        }
        if (content[0]) {
            vfs_write(fd, content, strlen(content));
            vfs_write(fd, "\n", 1);
        }
        vfs_close(fd);
        vga_puts_color("Appended to: ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts(fname); vga_puts("\n");
        auto_sync();
    } else {
        /* Create new file */
        if (content[0]) {
            uint32_t clen = strlen(content);
            /* Add newline at end */
            char* buf = (char*)kmalloc(clen + 2);
            memcpy(buf, content, clen);
            buf[clen] = '\n'; buf[clen+1] = '\0';
            vfs_create(fname, buf, clen + 1);
            kfree(buf);
        } else {
            vfs_create_empty(fname);
        }
        vga_puts_color("Created: ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts(fname); vga_puts("\n");
        auto_sync();
    }
}

static void cmd_thrr(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: thrr <filename.txt>\n", theme_error, VGA_BLACK);
        vga_puts("  Opens file in fullscreen editor.\n");
        vga_puts("  Creates new file if it doesn't exist.\n");
        return;
    }
    char fname[VFS_MAX_NAME];
    uint32_t fi = 0;
    while (arg[fi] && arg[fi] != ' ' && fi < VFS_MAX_NAME - 1) {
        fname[fi] = arg[fi]; fi++;
    }
    fname[fi] = '\0';
    editor_open(fname);
    auto_sync();  /* Persist any changes made in editor */
}

static void cmd_hdhf(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: hdhf <filename>\n", theme_error, VGA_BLACK);
        return;
    }
    /* Extract just the filename (first word) */
    char fname[VFS_MAX_NAME];
    uint32_t i = 0;
    while (arg[i] && arg[i] != ' ' && i < VFS_MAX_NAME - 1) {
        fname[i] = arg[i]; i++;
    }
    fname[i] = '\0';

    int32_t r = vfs_delete(fname);
    if (r == 0) {
        vga_puts_color("Deleted: ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts(fname); vga_puts("\n");
        auto_sync();
    } else if (r == -2) {
        vga_puts_color("Cannot delete: read-only file.\n", theme_error, VGA_BLACK);
    } else {
        vga_puts_color("File not found: ", theme_error, VGA_BLACK);
        vga_puts(fname); vga_puts("\n");
    }
}

static void cmd_smm(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: smm <old_name> <new_name>\n", theme_error, VGA_BLACK);
        return;
    }
    char old_name[VFS_MAX_NAME], new_name[VFS_MAX_NAME];
    uint32_t i = 0;
    while (arg[i] && arg[i] != ' ' && i < VFS_MAX_NAME - 1) {
        old_name[i] = arg[i]; i++;
    }
    old_name[i] = '\0';
    const char* nn = skip_spaces(arg + i);
    if (!nn[0]) {
        vga_puts_color("Provide new name.\n", theme_error, VGA_BLACK);
        return;
    }
    i = 0;
    while (nn[i] && nn[i] != ' ' && i < VFS_MAX_NAME - 1) {
        new_name[i] = nn[i]; i++;
    }
    new_name[i] = '\0';
    int32_t r = vfs_rename(old_name, new_name);
    if (r == 0) {
        vga_puts_color("Renamed: ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts(old_name); vga_puts(" -> "); vga_puts(new_name); vga_puts("\n");
        auto_sync();
    } else if (r == -2) {
        vga_puts_color("Cannot rename: read-only.\n", theme_error, VGA_BLACK);
    } else if (r == -3) {
        vga_puts_color("Target name already exists.\n", theme_error, VGA_BLACK);
    } else {
        vga_puts_color("File not found.\n", theme_error, VGA_BLACK);
    }
}

static void cmd_mlf(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: mlf <filename>\n", theme_error, VGA_BLACK);
        return;
    }
    const vfs_file_t* f = vfs_info(arg);
    if (!f) {
        vga_puts_color("File not found.\n", theme_error, VGA_BLACK);
        return;
    }
    vga_puts_color("=== File Info (mlf) ===\n", theme_header, VGA_BLACK);
    vga_puts("  Name:      "); vga_puts_color(f->name, VGA_WHITE, VGA_BLACK); vga_puts("\n");
    vga_puts("  Size:      "); vga_put_dec(f->size); vga_puts(" bytes\n");
    vga_puts("  Capacity:  "); vga_put_dec(f->capacity); vga_puts(" bytes\n");
    vga_puts("  Read-only: "); vga_puts(f->readonly ? "Yes" : "No"); vga_puts("\n");
    vga_puts("  Created:   ");
    uint32_t cs = f->created_tick / 100;
    vga_put_dec(cs / 60); vga_puts("m"); vga_put_dec(cs % 60); vga_puts("s after boot\n");
    vga_puts("  Modified:  ");
    uint32_t ms = f->modified_tick / 100;
    vga_put_dec(ms / 60); vga_puts("m"); vga_put_dec(ms % 60); vga_puts("s after boot\n");
    vga_puts("  Reads:     "); vga_put_dec(f->read_count);
    vga_puts(" ("); vga_put_dec(f->total_bytes_read); vga_puts("B)\n");
    vga_puts("  Writes:    "); vga_put_dec(f->write_count);
    vga_puts(" ("); vga_put_dec(f->total_bytes_written); vga_puts("B)\n");
}

static void cmd_nskhh(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: nskhh <source> <dest>\n", theme_error, VGA_BLACK);
        return;
    }
    char src[VFS_MAX_NAME], dst[VFS_MAX_NAME];
    uint32_t i = 0;
    while (arg[i] && arg[i] != ' ' && i < VFS_MAX_NAME - 1) { src[i] = arg[i]; i++; }
    src[i] = '\0';
    const char* d = skip_spaces(arg + i);
    if (!d[0]) { vga_puts_color("Provide dest name.\n", theme_error, VGA_BLACK); return; }
    i = 0;
    while (d[i] && d[i] != ' ' && i < VFS_MAX_NAME - 1) { dst[i] = d[i]; i++; }
    dst[i] = '\0';
    if (!vfs_exists(src)) { vga_puts_color("Source not found.\n", theme_error, VGA_BLACK); return; }
    if (vfs_exists(dst)) { vga_puts_color("Dest already exists.\n", theme_error, VGA_BLACK); return; }
    int32_t fd = vfs_open(src);
    if (fd < 0) return;
    /* Use a heap buffer, not the stack: VFS_FILE_MAXSZ (64KB) far exceeds the
     * kernel stack and would smash it. */
    uint8_t* tmp = (uint8_t*)kmalloc(VFS_FILE_MAXSZ);
    if (!tmp) {
        vfs_close(fd);
        vga_puts_color("Out of memory.\n", theme_error, VGA_BLACK);
        return;
    }
    int32_t n = vfs_read(fd, tmp, VFS_FILE_MAXSZ);
    vfs_close(fd);
    if (n > 0) vfs_create(dst, tmp, n);
    else vfs_create_empty(dst);
    kfree(tmp);
    vga_puts_color("Copied: ", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(src); vga_puts(" -> "); vga_puts(dst); vga_puts("\n");
}

static void cmd_nz(void) {
    vga_puts_color("=== System Info (nz) ===\n", theme_header, VGA_BLACK);
    vga_puts("  Kernel:  ");
    vga_puts_color("AIOS Kernel v2.0.0\n", theme_value, VGA_BLACK);
    vga_puts("  Shell:   ");
    vga_puts_color("AIOS Shell (Arabic commands)\n", theme_value, VGA_BLACK);
    vga_puts("  CPU:     ");
    vga_puts_color(g_cpu_info.vendor, theme_value, VGA_BLACK);
    if (g_cpu_info.has_sse) vga_puts(" SSE");
    if (g_cpu_info.has_apic) vga_puts(" APIC");
    if (g_cpu_info.has_pae) vga_puts(" PAE");
    vga_puts("\n");
    vga_puts("  Memory:  ");
    vga_put_dec(g_boot_info.total_memory_kb / 1024);
    vga_puts(" MB total\n");
    vga_puts("  Uptime:  ");
    uint32_t up = pit_get_uptime();
    vga_put_dec(up / 60);
    vga_puts("m ");
    vga_put_dec(up % 60);
    vga_puts("s\n");
    vga_puts("  Ticks:   ");
    vga_put_dec(pit_get_ticks());
    vga_puts("\n");
    rtc_dump();
    mouse_dump();
}

static void cmd_am(void) {
    proc_dump();
}

static void cmd_zk(void) {
    vga_puts_color("=== Memory (zk) ===\n", theme_header, VGA_BLACK);
    pmm_dump();
    vmm_dump();
    heap_dump();
}

static void cmd_dk(void) {
    ai_sample_perf();
    ai_export_report();
    devtrack_update();
    devtrack_report();
}

static void cmd_ms(void) {
    mouse_cursor_hide();
    vga_clear();
}

static void cmd_wqf(void) {
    vga_puts_color("\nSystem halting... (wqf)\n", VGA_YELLOW, VGA_BLACK);
    vga_puts_color("Ma'a salama!\n", VGA_LIGHT_GREEN, VGA_BLACK);
    pit_sleep_ms(500);
    __asm__ volatile("cli; hlt");
}

/* Simple string to uint32 */
static uint32_t parse_num(const char* s) {
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

static void cmd_qrs(void) {
    ata_dump();
}

static void cmd_qra(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0] || arg[0] < '0' || arg[0] > '9') {
        vga_puts_color("Usage: qra <sector_number>\n", theme_error, VGA_BLACK);
        return;
    }
    uint32_t lba = parse_num(arg);
    uint8_t buf[512];

    vga_puts_color("=== Read Sector ", theme_header, VGA_BLACK);
    vga_put_dec(lba);
    vga_puts_color(" ===\n", theme_header, VGA_BLACK);

    if (ata_read_sector(lba, buf) != 0) {
        vga_puts_color("  Read failed!\n", theme_error, VGA_BLACK);
        return;
    }

    /* Show as text (printable chars only) */
    vga_puts_color("  Text: ", VGA_DARK_GREY, VGA_BLACK);
    bool has_text = false;
    for (int i = 0; i < 512 && i < 200; i++) {
        if (buf[i] >= 32 && buf[i] < 127) {
            vga_putchar(buf[i]);
            has_text = true;
        } else if (buf[i] == '\n') {
            vga_puts("\n        ");
            has_text = true;
        } else if (has_text && buf[i] == 0) {
            break;  /* Null terminator after text */
        }
    }
    if (!has_text) vga_puts("(empty/binary data)");
    vga_puts("\n");

    /* Show hex dump of first 64 bytes */
    vga_puts_color("  Hex:  ", VGA_DARK_GREY, VGA_BLACK);
    for (int i = 0; i < 32; i++) {
        const char hex[] = "0123456789ABCDEF";
        vga_putchar(hex[buf[i] >> 4]);
        vga_putchar(hex[buf[i] & 0xF]);
        vga_putchar(' ');
    }
    vga_puts("...\n");
}

static void cmd_hfz(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0] || arg[0] < '0' || arg[0] > '9') {
        vga_puts_color("Usage: hfz <sector> <text>\n", theme_error, VGA_BLACK);
        return;
    }

    /* Parse sector number */
    uint32_t lba = parse_num(arg);
    while (*arg >= '0' && *arg <= '9') arg++;
    const char* text = skip_spaces(arg);

    if (!text[0]) {
        vga_puts_color("  No text provided.\n", theme_error, VGA_BLACK);
        return;
    }

    /* Prepare sector buffer: fill with text, pad with zeros */
    uint8_t buf[512];
    memset(buf, 0, 512);
    uint32_t tlen = strlen(text);
    if (tlen > 510) tlen = 510;
    memcpy(buf, text, tlen);
    buf[tlen] = '\n';  /* Newline at end */

    vga_puts("  Writing to sector ");
    vga_put_dec(lba);
    vga_puts(": ");
    vga_put_dec(tlen);
    vga_puts(" bytes... ");

    if (ata_write_sector(lba, buf) == 0) {
        vga_puts_color("[OK]\n", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("  Data saved to disk! Persists after reboot.\n");
    } else {
        vga_puts_color("[FAILED]\n", theme_error, VGA_BLACK);
    }
}

/* Preemptive scheduling demo threads — NO yield() calls! */
static volatile bool demo_running = false;

static void preempt_thread_x(void) {
    for (int i = 0; i < 20 && demo_running; i++) {
        vga_puts_color("[X]", VGA_LIGHT_RED, VGA_BLACK);
        /* Busy wait — no yield! Timer preempts us. */
        for (volatile int d = 0; d < 500000; d++);
    }
}

static void preempt_thread_y(void) {
    for (int i = 0; i < 20 && demo_running; i++) {
        vga_puts_color("[Y]", VGA_LIGHT_GREEN, VGA_BLACK);
        for (volatile int d = 0; d < 500000; d++);
    }
}

static void preempt_thread_z(void) {
    for (int i = 0; i < 20 && demo_running; i++) {
        vga_puts_color("[Z]", VGA_LIGHT_CYAN, VGA_BLACK);
        for (volatile int d = 0; d < 500000; d++);
    }
}

static void cmd_jrb(void) {
    vga_puts_color("=== Preemptive Scheduling Test (jrb) ===\n", theme_header, VGA_BLACK);
    vga_puts("  Launching 3 threads with NO yield() calls.\n");
    vga_puts("  Timer preempts them every 50ms (5 ticks).\n");
    vga_puts("  If you see [X][Y][Z] interleaving = preemptive works!\n\n  ");

    demo_running = true;
    proc_create("preempt_X", preempt_thread_x, 128);
    proc_create("preempt_Y", preempt_thread_y, 128);
    proc_create("preempt_Z", preempt_thread_z, 128);

    /* Wait for threads to finish */
    pit_sleep_ms(3000);
    demo_running = false;
    pit_sleep_ms(500);

    vga_puts("\n\n  Test complete. If threads interleaved without\n");
    vga_puts("  yield() = preemptive scheduling is working!\n");
}

static void cmd_mft(void) {
    vga_puts_color("=== Key Test (mft) ===\n", theme_header, VGA_BLACK);
    vga_puts("  Press keys to see codes. ESC to stop.\n");
    vga_puts("  Try: letters, Enter, arrows, Ctrl+P\n  ");
    keyboard_set_echo(false);
    while (1) {
        if (!keyboard_has_char()) { __asm__ volatile("hlt"); continue; }
        char c = keyboard_getchar();
        if (c == 27) break;  /* ESC */
        vga_puts("[");
        if (c == KEY_UP)        vga_puts_color("UP", VGA_LIGHT_GREEN, VGA_BLACK);
        else if (c == KEY_DOWN) vga_puts_color("DN", VGA_LIGHT_GREEN, VGA_BLACK);
        else if (c == KEY_LEFT) vga_puts_color("LT", VGA_LIGHT_GREEN, VGA_BLACK);
        else if (c == KEY_RIGHT)vga_puts_color("RT", VGA_LIGHT_GREEN, VGA_BLACK);
        else if (c == '\n')     vga_puts("ENT");
        else if (c == '\b')     vga_puts("BS");
        else if (c == '\t')     vga_puts("TAB");
        else if (c == 19)       vga_puts("C-S");
        else if (c >= ' ' && c < 127) { vga_putchar(c); }
        else { vga_puts("#"); vga_put_dec((uint32_t)(uint8_t)c); }
        vga_puts("=");
        vga_put_dec((uint32_t)(uint8_t)c);
        vga_puts("] ");
    }
    keyboard_set_echo(true);
    vga_puts("\n  Last scancode: ");
    vga_put_hex(g_kb_stats.last_scancode);
    vga_puts("\n");
}

/* === Feature #5: Kill process by PID === */
static void cmd_aqtl(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0] || arg[0] < '0' || arg[0] > '9') {
        vga_puts_color("Usage: aqtl <pid> [signal]\n", theme_error, VGA_BLACK);
        vga_puts("  Sends a signal to a process. Default: SIGTERM (15)\n");
        vga_puts("  Signals: 1=HUP 2=INT 9=KILL 10=USR1 12=USR2\n");
        vga_puts("           15=TERM 17=STOP 18=CONT\n");
        vga_puts("  Cannot kill PID 0 (kernel).\n");
        return;
    }

    /* Parse PID */
    uint32_t pid = parse_num(arg);
    if (pid == 0) {
        vga_puts_color("  Cannot signal kernel (PID 0)!\n", theme_error, VGA_BLACK);
        return;
    }

    /* Parse optional signal number */
    int signum = SIGTERM; /* Default */
    const char* s = arg;
    while (*s && *s != ' ') s++;
    s = skip_spaces(s);
    if (*s >= '0' && *s <= '9') {
        signum = (int)parse_num(s);
    }

    process_t* p = proc_find(pid);
    if (!p) {
        vga_printf("  PID %u not found.\n", pid);
        return;
    }

    const char* was_state = (p->state == PROC_TERMINATED) ? "TERM" :
                            (p->state == PROC_STOPPED) ? "STOP" :
                            (p->state == PROC_RUNNING) ? "RUN" : "READY";
    char pname[32];
    memcpy(pname, p->name, 32);

    vga_printf("  Sending %s(%d) to PID %u (%s) [%s]... ",
               signal_name(signum), signum, pid, pname, was_state);

    int32_t r = proc_signal(pid, signum);
    if (r == 0) {
        if (signum == SIGKILL)
            vga_puts_color("[KILLED]\n", VGA_LIGHT_GREEN, VGA_BLACK);
        else if (signum == SIGSTOP)
            vga_puts_color("[STOPPED]\n", VGA_YELLOW, VGA_BLACK);
        else if (signum == SIGCONT)
            vga_puts_color("[CONTINUED]\n", VGA_LIGHT_GREEN, VGA_BLACK);
        else
            vga_puts_color("[SENT]\n", VGA_LIGHT_GREEN, VGA_BLACK);
    } else {
        vga_puts_color("[FAILED]\n", theme_error, VGA_BLACK);
    }
}

/* ishara (إشارة = signal) — signal info and demo */
static void cmd_ishara(const char* arg) {
    arg = skip_spaces(arg);

    /* ishara demo — interactive signal demo */
    if (strncmp(arg, "demo", 4) == 0) {
        vga_puts_color("=== Signal Demo ===\n", theme_header, VGA_BLACK);
        vga_puts("  Creating test process...\n");

        /* Create worker — will be stopped before it runs,
         * so we use proc_exit as the entry (safe fallback) */
        int32_t worker_pid = proc_create("sig_worker", proc_exit, 128);
        if (worker_pid < 0) {
            vga_puts_color("  Failed to create worker!\n", theme_error, VGA_BLACK);
            return;
        }

        /* We can't easily set up a handler in a separate process context
         * from here, so demonstrate stop/continue/kill instead */
        vga_printf("  Worker PID %u created.\n", (uint32_t)worker_pid);

        /* Stop it */
        vga_puts("  Sending SIGSTOP... ");
        proc_signal((uint32_t)worker_pid, SIGSTOP);
        process_t* w = proc_find((uint32_t)worker_pid);
        if (w && w->state == PROC_STOPPED) {
            vga_puts_color("STOPPED\n", VGA_YELLOW, VGA_BLACK);
        }

        /* Show process list */
        vga_puts("  Process state:\n");
        proc_dump();

        /* Continue it */
        vga_puts("\n  Sending SIGCONT... ");
        proc_signal((uint32_t)worker_pid, SIGCONT);
        w = proc_find((uint32_t)worker_pid);
        if (w && w->state == PROC_READY) {
            vga_puts_color("CONTINUED\n", VGA_LIGHT_GREEN, VGA_BLACK);
        }

        /* Kill it */
        vga_puts("  Sending SIGKILL... ");
        proc_signal((uint32_t)worker_pid, SIGKILL);
        w = proc_find((uint32_t)worker_pid);
        if (!w) {
            vga_puts_color("KILLED\n", VGA_LIGHT_GREEN, VGA_BLACK);
        }

        vga_puts_color("\n  Signal demo complete.\n", VGA_LIGHT_GREEN, VGA_BLACK);
        return;
    }

    /* ishara (no arg) — show signal status of all processes */
    vga_puts_color("=== Signals (ishara) ===\n", theme_header, VGA_BLACK);
    vga_puts("  Available signals:\n");
    vga_puts("    1=SIGHUP   2=SIGINT   9=SIGKILL  10=SIGUSR1\n");
    vga_puts("   12=SIGUSR2 15=SIGTERM  17=SIGSTOP  18=SIGCONT\n\n");

    bool found = false;
    for (uint32_t pid = 0; pid < 100; pid++) {
        process_t* p = proc_find(pid);
        if (!p) continue;
        found = true;

        vga_printf("  PID %u (%s): ", pid, p->name);
        switch (p->state) {
            case PROC_RUNNING: vga_puts_color("RUN ", VGA_LIGHT_GREEN, VGA_BLACK); break;
            case PROC_READY:   vga_puts_color("RDY ", VGA_YELLOW, VGA_BLACK); break;
            case PROC_STOPPED: vga_puts_color("STP ", VGA_LIGHT_RED, VGA_BLACK); break;
            default: vga_puts("??? "); break;
        }
        vga_printf(" delivered=%u caught=%u ignored=%u",
                   p->sig.delivered, p->sig.caught, p->sig.ignored);
        if (p->sig.pending) {
            vga_puts(" pending=");
            vga_put_hex(p->sig.pending);
        }
        vga_puts("\n");
    }
    if (!found) vga_puts("  No active processes.\n");

    vga_printf("\n  Total signals sent: %u  Stopped processes: %u\n",
               g_sched_stats.total_signals, g_sched_stats.total_stopped);
    vga_puts("  Usage: aqtl <pid> [signal]  |  ishara demo\n");
}

/* sbrk demo — test per-process heap allocation */
static void cmd_sbrk_demo(void) {
    vga_puts_color("=== sbrk Demo (thkra) ===\n", theme_header, VGA_BLACK);

    vga_puts("  Testing per-process heap allocation...\n\n");

    /* Get current break */
    void* brk0 = proc_sbrk(0);
    vga_printf("  Initial break: 0x%x\n", (uint32_t)brk0);

    /* Allocate 256 bytes */
    void* p1 = proc_sbrk(256);
    if ((uint32_t)p1 == (uint32_t)-1) {
        vga_puts_color("  sbrk(256) FAILED\n", theme_error, VGA_BLACK);
        return;
    }
    vga_printf("  sbrk(256) = 0x%x (got 256 bytes)\n", (uint32_t)p1);

    /* Write to allocated memory */
    char* mem = (char*)p1;
    const char* test_str = "Hello from sbrk!";
    for (int i = 0; test_str[i]; i++) mem[i] = test_str[i];
    mem[16] = '\0';

    vga_printf("  Wrote: \"%s\" at 0x%x\n", mem, (uint32_t)mem);

    /* Allocate more */
    void* p2 = proc_sbrk(512);
    vga_printf("  sbrk(512) = 0x%x (got 512 more bytes)\n", (uint32_t)p2);

    /* Verify original data is intact */
    vga_printf("  Original data: \"%s\" (intact: %s)\n",
               mem, (mem[0] == 'H') ? "YES" : "NO");

    /* Show current break */
    void* brk_now = proc_sbrk(0);
    vga_printf("  Current break: 0x%x\n", (uint32_t)brk_now);
    vga_printf("  Total heap used: %u bytes\n",
               (uint32_t)brk_now - (uint32_t)brk0);

    /* Show process memory info */
    if (current_process) {
        vga_puts("\n  Process memory:\n");
        vga_printf("    Heap start: 0x%x\n", current_process->mem.heap_start);
        vga_printf("    Heap break: 0x%x\n", current_process->mem.heap_break);
        vga_printf("    Heap max:   0x%x\n", current_process->mem.heap_max);
        vga_printf("    Allocated:  %u bytes\n", current_process->mem.total_allocated);
        vga_printf("    sbrk calls: %u\n", current_process->mem.sbrk_calls);
    }

    vga_puts_color("\n  sbrk demo complete.\n", VGA_LIGHT_GREEN, VGA_BLACK);
}

/* wqt (وقت = time) — show date and time */
static void cmd_time(void) {
    vga_puts_color("=== Date/Time (wqt) ===\n", theme_header, VGA_BLACK);
    rtc_time_t t;
    rtc_read(&t);
    char buf[24];
    rtc_format(&t, buf, sizeof(buf));
    vga_puts("  ");
    vga_puts_color(buf, VGA_WHITE, VGA_BLACK);
    vga_puts("\n");
    vga_puts("  Unix (approx): ");
    vga_put_dec(rtc_unix_approx());
    vga_puts("\n");
}

/* mjld (مجلد = folder/directory) — create directory */
static void cmd_mkdir(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: mjld <dirname>\n", theme_error, VGA_BLACK);
        return;
    }
    int32_t r = vfs_mkdir(arg);
    if (r >= 0) {
        vga_printf("  Created directory: %s/\n", arg);
    } else if (r == -2) {
        vga_puts_color("  Already exists: ", theme_error, VGA_BLACK); vga_puts(arg); vga_puts("\n");
    } else {
        vga_puts_color("  Failed to create directory.\n", theme_error, VGA_BLACK);
    }
}

/* dkhl (دخل = enter) — change directory */
static void cmd_cd(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0] || strcmp(arg, "/") == 0) {
        vfs_chdir("/");
        vga_puts("  /\n");
        return;
    }
    int32_t r = vfs_chdir(arg);
    if (r == 0) {
        vga_printf("  %s\n", vfs_getcwd());
    } else {
        vga_puts_color("  Not a directory: ", theme_error, VGA_BLACK); vga_puts(arg); vga_puts("\n");
    }
}

/* pwd — print working directory */
static void cmd_pwd(void) {
    vga_puts("  ");
    vga_puts_color(vfs_getcwd(), VGA_YELLOW, VGA_BLACK);
    vga_puts("\n");
}

/* === Feature #1: Persistent filesystem commands === */
static void cmd_tnsyq(void) {
    vga_puts_color("=== Format Disk (tnsyq) ===\n", theme_header, VGA_BLACK);
    if (!g_ata_disk.present) {
        vga_puts_color("  No disk attached! Use: -hda disk.img\n", theme_error, VGA_BLACK);
        return;
    }
    vga_puts_color("  WARNING: This will erase ALL data on disk!\n", VGA_YELLOW, VGA_BLACK);
    vga_puts("  Type 'y' to confirm: ");
    keyboard_set_echo(false);
    while (!keyboard_has_char()) __asm__ volatile("hlt");
    char c = keyboard_getchar();
    keyboard_set_echo(true);
    vga_putchar(c);
    vga_puts("\n");
    if (c != 'y' && c != 'Y') {
        vga_puts("  Cancelled.\n");
        return;
    }
    aiosfs_format();
}

static void cmd_hfzk(void) {
    vga_puts_color("=== Sync to Disk (hfzk) ===\n", theme_header, VGA_BLACK);
    aiosfs_sync();
}

static void cmd_hmml(void) {
    vga_puts_color("=== Load from Disk (hmml) ===\n", theme_header, VGA_BLACK);
    aiosfs_load();
}

static void cmd_dskfs(void) {
    aiosfs_dump();
}

/* === Feature #6: Background process demo === */
/* khlf (خلف khalaf = background) — launches a process that prints while shell is active */
static volatile bool bg_running = false;
static void bg_task_fn(void) {
    for (int i = 1; i <= 10 && bg_running; i++) {
        vga_puts_color("[BG:", VGA_LIGHT_MAGENTA, VGA_BLACK);
        vga_put_dec(i);
        vga_puts_color("] ", VGA_LIGHT_MAGENTA, VGA_BLACK);
        /* Wait ~1 second between prints */
        pit_sleep_ms(1000);
    }
    bg_running = false;
}

static void cmd_khlf(void) {
    vga_puts_color("=== Background Task (khlf) ===\n", theme_header, VGA_BLACK);
    vga_puts("  Launching background process (prints [BG:N] every second).\n");
    vga_puts("  Try typing commands while it runs!\n");
    vga_puts("  Use 'am' to see it, 'aqtl <pid>' to stop it.\n\n");
    bg_running = true;
    int32_t pid = proc_create("background", bg_task_fn, 128);
    if (pid < 0) {
        vga_puts_color("  Failed to create background process!\n", theme_error, VGA_BLACK);
        bg_running = false;
        return;
    }
    vga_printf("  Started PID %d. Shell remains responsive.\n", pid);
}

/* === Feature #8: Panic test command === */
/* fzh (فزع faza' = panic) — trigger a kernel panic for testing */
static void cmd_fzh(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("=== Panic Test (fzh) ===\n", theme_header, VGA_BLACK);
        vga_puts("  Triggers a kernel panic to test the panic screen.\n");
        vga_puts_color("  WARNING: This will halt the system!\n", VGA_YELLOW, VGA_BLACK);
        vga_puts("  Options:\n");
        vga_puts("    fzh 1  — Divide by zero (Exception #0)\n");
        vga_puts("    fzh 2  — Page fault (Exception #14)\n");
        vga_puts("    fzh 3  — Manual kpanic() call\n");
        return;
    }

    vga_puts_color("  Triggering panic in 1 second...\n", VGA_LIGHT_RED, VGA_BLACK);
    pit_sleep_ms(1000);

    if (arg[0] == '1') {
        /* Divide by zero */
        volatile int a = 1;
        volatile int b = 0;
        volatile int c = a / b;
        (void)c;
    } else if (arg[0] == '2') {
        /* Page fault — read from unmapped address */
        volatile uint32_t* bad_ptr = (volatile uint32_t*)0xDEAD0000;
        volatile uint32_t val = *bad_ptr;
        (void)val;
    } else if (arg[0] == '3') {
        /* Manual panic */
        kpanic("Manual panic triggered by 'fzh 3' command");
    } else {
        vga_puts_color("  Unknown option. Use: fzh 1, fzh 2, or fzh 3\n", theme_error, VGA_BLACK);
    }
}

/* === Feature #10: Device commands === */
/* ajhz (أجهزة ajhiza = devices) — list all devices */
static void cmd_ajhz(void) {
    dev_dump();
}

/* dev r <name> [count] — read from device
 * dev w <name> <text>  — write to device */
static void cmd_dev(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0] || (arg[0] != 'r' && arg[0] != 'w')) {
        vga_puts_color("=== Device I/O (dev) ===\n", theme_header, VGA_BLACK);
        vga_puts("  dev r <name> [count]  — read from device\n");
        vga_puts("  dev w <name> <text>   — write to device\n");
        vga_puts("  Examples:\n");
        vga_puts("    dev r uptime          read uptime\n");
        vga_puts("    dev r random 16       read 16 random bytes\n");
        vga_puts("    dev w serial Hello    write to COM1\n");
        vga_puts("    dev w console Test    write to screen\n");
        vga_puts("    dev w null discard    write to /dev/null\n");
        return;
    }

    char op = arg[0];
    arg = skip_spaces(arg + 1);

    /* Extract device name */
    char dname[DEV_NAME_MAX];
    uint32_t di = 0;
    while (arg[di] && arg[di] != ' ' && di < DEV_NAME_MAX - 1) {
        dname[di] = arg[di]; di++;
    }
    dname[di] = '\0';

    if (!dname[0]) {
        vga_puts_color("  Specify device name.\n", theme_error, VGA_BLACK);
        return;
    }

    /* Check device exists */
    device_t* d = dev_find(dname);
    if (!d) {
        vga_printf("  Device '%s' not found. Use 'ajhz' to list.\n", dname);
        return;
    }

    if (op == 'r') {
        /* Read */
        if (!d->ops.read) {
            vga_printf("  /dev/%s does not support read.\n", dname);
            return;
        }
        const char* count_str = skip_spaces(arg + di);
        uint32_t count = 64; /* default */
        if (count_str[0] >= '0' && count_str[0] <= '9') {
            count = parse_num(count_str);
        }
        if (count > 512) count = 512;

        uint8_t buf[512];
        memset(buf, 0, sizeof(buf));
        int32_t n = dev_read(dname, buf, count);
        if (n < 0) {
            vga_puts_color("  Read failed.\n", theme_error, VGA_BLACK);
            return;
        }
        if (n == 0) {
            vga_puts("  (empty — 0 bytes read)\n");
            return;
        }

        vga_printf("  /dev/%s: %d bytes:\n  ", dname, n);

        /* Show as text if printable, hex otherwise */
        bool is_text = true;
        for (int32_t i = 0; i < n; i++) {
            if (buf[i] != '\n' && buf[i] != '\t' && (buf[i] < 32 || buf[i] > 126)) {
                is_text = false; break;
            }
        }

        if (is_text) {
            for (int32_t i = 0; i < n; i++) vga_putchar(buf[i]);
            if (n > 0 && buf[n-1] != '\n') vga_puts("\n");
        } else {
            /* Hex dump */
            const char hex[] = "0123456789ABCDEF";
            for (int32_t i = 0; i < n; i++) {
                vga_putchar(hex[buf[i] >> 4]);
                vga_putchar(hex[buf[i] & 0xF]);
                vga_putchar(' ');
                if ((i + 1) % 16 == 0 && i + 1 < n) vga_puts("\n  ");
            }
            vga_puts("\n");
        }
    } else {
        /* Write */
        if (!d->ops.write) {
            vga_printf("  /dev/%s does not support write.\n", dname);
            return;
        }
        const char* text = skip_spaces(arg + di);
        if (!text[0]) {
            vga_puts_color("  Specify text to write.\n", theme_error, VGA_BLACK);
            return;
        }
        uint32_t len = strlen(text);
        int32_t n = dev_write(dname, text, len);
        if (n < 0) {
            vga_puts_color("  Write failed.\n", theme_error, VGA_BLACK);
        } else {
            vga_printf("  Wrote %d bytes to /dev/%s\n", n, dname);
        }
    }
}

/* === Feature #12: Pipe/IPC commands === */
/* anbb (أنبوب unbub = pipe) — pipe operations */
static void cmd_anbb(const char* arg) {
    arg = skip_spaces(arg);

    /* No args = list pipes */
    if (!arg[0]) {
        pipe_dump();
        return;
    }

    /* anbb demo */
    if (strncmp(arg, "demo", 4) == 0) {
        pipe_demo();
        return;
    }

    /* anbb mk <name> */
    if (strncmp(arg, "mk", 2) == 0 && (arg[2] == ' ' || arg[2] == '\0')) {
        const char* name = skip_spaces(arg + 2);
        if (!name[0]) {
            vga_puts_color("Usage: anbb mk <name>\n", theme_error, VGA_BLACK);
            return;
        }
        char pname[PIPE_NAME_MAX];
        uint32_t i = 0;
        while (name[i] && name[i] != ' ' && i < PIPE_NAME_MAX - 1) {
            pname[i] = name[i]; i++;
        }
        pname[i] = '\0';

        int32_t r = pipe_create(pname);
        if (r >= 0) {
            vga_printf("  Pipe '%s' created (slot %d)\n", pname, r);
        } else if (r == -2) {
            vga_printf("  Pipe '%s' already exists.\n", pname);
        } else {
            vga_puts_color("  Failed (max 8 pipes).\n", theme_error, VGA_BLACK);
        }
        return;
    }

    /* anbb rm <name> */
    if (strncmp(arg, "rm", 2) == 0 && (arg[2] == ' ' || arg[2] == '\0')) {
        const char* name = skip_spaces(arg + 2);
        if (!name[0]) {
            vga_puts_color("Usage: anbb rm <name>\n", theme_error, VGA_BLACK);
            return;
        }
        char pname[PIPE_NAME_MAX];
        uint32_t i = 0;
        while (name[i] && name[i] != ' ' && i < PIPE_NAME_MAX - 1) {
            pname[i] = name[i]; i++;
        }
        pname[i] = '\0';

        if (pipe_destroy(pname) == 0) {
            vga_printf("  Pipe '%s' destroyed.\n", pname);
        } else {
            vga_printf("  Pipe '%s' not found.\n", pname);
        }
        return;
    }

    /* anbb w <name> <text> */
    if (arg[0] == 'w' && arg[1] == ' ') {
        arg = skip_spaces(arg + 2);
        char pname[PIPE_NAME_MAX];
        uint32_t i = 0;
        while (arg[i] && arg[i] != ' ' && i < PIPE_NAME_MAX - 1) {
            pname[i] = arg[i]; i++;
        }
        pname[i] = '\0';
        const char* text = skip_spaces(arg + i);

        pipe_t* p = pipe_find(pname);
        if (!p) {
            vga_printf("  Pipe '%s' not found.\n", pname);
            return;
        }
        if (!text[0]) {
            vga_puts_color("  Provide text to write.\n", theme_error, VGA_BLACK);
            return;
        }
        uint32_t len = strlen(text);
        int32_t n = pipe_write(p, text, len);
        /* Also write a newline */
        pipe_write(p, "\n", 1);
        if (n >= 0) {
            vga_printf("  Wrote %d bytes to pipe '%s' (%u buffered)\n",
                       n + 1, pname, p->count);
        } else {
            vga_puts_color("  Write failed (pipe full?).\n", theme_error, VGA_BLACK);
        }
        return;
    }

    /* anbb r <name> */
    if (arg[0] == 'r' && (arg[1] == ' ' || arg[1] == '\0')) {
        const char* name = skip_spaces(arg + 1);
        if (!name[0]) {
            vga_puts_color("Usage: anbb r <name>\n", theme_error, VGA_BLACK);
            return;
        }
        char pname[PIPE_NAME_MAX];
        uint32_t i = 0;
        while (name[i] && name[i] != ' ' && i < PIPE_NAME_MAX - 1) {
            pname[i] = name[i]; i++;
        }
        pname[i] = '\0';

        pipe_t* p = pipe_find(pname);
        if (!p) {
            vga_printf("  Pipe '%s' not found.\n", pname);
            return;
        }
        if (p->count == 0) {
            vga_printf("  Pipe '%s' is empty.\n", pname);
            return;
        }
        char buf[256];
        int32_t n = pipe_read(p, buf, 255);
        if (n > 0) {
            buf[n] = '\0';
            vga_printf("  Read %d bytes from '%s': ", n, pname);
            vga_puts(buf);
            if (n > 0 && buf[n-1] != '\n') vga_puts("\n");
        } else {
            vga_puts("  (empty)\n");
        }
        return;
    }

    /* Unknown subcommand */
    vga_puts_color("=== Pipe Commands (anbb) ===\n", theme_header, VGA_BLACK);
    vga_puts("  anbb            List active pipes\n");
    vga_puts("  anbb mk <n>     Create named pipe\n");
    vga_puts("  anbb rm <n>     Destroy pipe\n");
    vga_puts("  anbb w <n> <t>  Write text to pipe\n");
    vga_puts("  anbb r <n>      Read from pipe\n");
    vga_puts("  anbb demo       Producer/consumer demo\n");
}

/* === Command Dispatcher === */
static void shell_exec(const char* line) {
    const char* cmd = skip_spaces(line);
    if (!cmd[0]) return;  /* Empty line */

    if (strcmp(cmd, "sd") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "db") == 0) {
        cmd_db();
    } else if (strncmp(cmd, "rq", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) {
        cmd_rq(cmd + 2);
    } else if (strncmp(cmd, "ktb", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        cmd_ktb(cmd + 3);
    } else if (strncmp(cmd, "thrr", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        cmd_thrr(cmd + 4);
    } else if (strncmp(cmd, "hdhf", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        cmd_hdhf(skip_spaces(cmd + 4));
    } else if (strncmp(cmd, "smm", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        cmd_smm(cmd + 3);
    } else if (strncmp(cmd, "mlf", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        cmd_mlf(skip_spaces(cmd + 3));
    } else if (strncmp(cmd, "nskhh", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\0')) {
        cmd_nskhh(cmd + 5);
    } else if (strcmp(cmd, "nz") == 0) {
        cmd_nz();
    } else if (strcmp(cmd, "am") == 0) {
        cmd_am();
    } else if (strcmp(cmd, "zk") == 0) {
        cmd_zk();
    } else if (strcmp(cmd, "dk") == 0) {
        cmd_dk();
    } else if (strcmp(cmd, "ms") == 0) {
        cmd_ms();
    } else if (strcmp(cmd, "wqf") == 0) {
        cmd_wqf();
    } else if (strncmp(cmd, "shgl", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        const char* arg = skip_spaces(cmd + 4);
        if (!arg[0]) {
            vga_puts_color("Usage: shgl <elf_file>\n", theme_error, VGA_BLACK);
            vga_puts_color("  Try: shgl hello.elf\n", VGA_DARK_GREY, VGA_BLACK);
        } else {
            elf_load_and_run(arg);
        }
    } else if (strcmp(cmd, "qrs") == 0) {
        cmd_qrs();
    } else if (strncmp(cmd, "qra", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        cmd_qra(cmd + 3);
    } else if (strncmp(cmd, "hfz", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        cmd_hfz(cmd + 3);
    } else if (strcmp(cmd, "rsm") == 0) {
        gfx_demo();
    } else if (strcmp(cmd, "jrb") == 0) {
        cmd_jrb();
    } else if (strcmp(cmd, "mft") == 0) {
        cmd_mft();
    } else if (strcmp(cmd, "dka") == 0) {
        ai_agent_status();
    } else if (strcmp(cmd, "tnb") == 0) {
        ai_agent_predictions();
    } else if (strcmp(cmd, "thll") == 0) {
        ai_agent_deep_analysis();
    } else if (strcmp(cmd, "rsd") == 0) {
        ai_agent_monitor();
    } else if (strcmp(cmd, "shbk") == 0) {
        net_dump();
    } else if (strcmp(cmd, "pci") == 0) {
        pci_dump();
    } else if (strcmp(cmd, "nfdh") == 0) {
        wm_run();
    } else if (strcmp(cmd, "nwat") == 0) {
        smp_dump();
    } else if (strcmp(cmd, "qfl") == 0) {
        smp_test_locks();
    } else if (strcmp(cmd, "mwzy") == 0) {
        smp_parallel();
    } else if (strncmp(cmd, "irsl", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        const char* msg = skip_spaces(cmd + 4);
        if (!msg[0]) {
            vga_puts_color("Usage: irsl <message>\n", theme_error, VGA_BLACK);
        } else {
            ip_addr_t dst = {{10, 0, 2, 2}};  /* QEMU gateway */
            vga_puts("  Sending UDP to 10.0.2.2:1234: \"");
            vga_puts(msg); vga_puts("\"\n");
            if (net_send_udp(dst, 1234, 5000, msg, strlen(msg)) == 0) {
                vga_puts_color("  Sent! ", VGA_LIGHT_GREEN, VGA_BLACK);
                vga_put_dec(strlen(msg)); vga_puts(" bytes\n");
            } else {
                vga_puts_color("  Send failed.\n", theme_error, VGA_BLACK);
            }
        }
    } else if (strcmp(cmd, "stlm") == 0) {
        vga_puts_color("=== Received Packets (stlm) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
        /* Process any echo/ARP packets first */
        net_poll();
        net_packet_t* pkt = net_receive();
        if (!pkt) {
            vga_puts("  No packets in buffer.\n");
        } else {
            vga_puts("  Packet: "); vga_put_dec(pkt->length); vga_puts(" bytes\n");
            vga_puts("  Hex: ");
            for (uint32_t i = 0; i < pkt->length && i < 40; i++) {
                const char hex[] = "0123456789ABCDEF";
                vga_putchar(hex[pkt->data[i] >> 4]);
                vga_putchar(hex[pkt->data[i] & 0xF]);
                vga_putchar(' ');
            }
            if (pkt->length > 40) vga_puts("...");
            vga_puts("\n");
            net_process_rx(pkt);
            net_rx_consume();
        }
    } else if (strncmp(cmd, "sma", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        vga_puts_color("=== UDP Echo Server (sma) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
        const char* arg = skip_spaces(cmd + 3);
        if (strcmp(arg, "stop") == 0 || strcmp(arg, "off") == 0) {
            net_echo_stop();
        } else if (g_echo.running) {
            vga_puts("  Status: ");
            vga_puts_color("RUNNING", VGA_LIGHT_GREEN, VGA_BLACK);
            vga_puts(" on port "); vga_put_dec(g_echo.port);
            vga_puts("\n  Echoed: "); vga_put_dec(g_echo.packets_echoed);
            vga_puts(" packets ("); vga_put_dec(g_echo.bytes_echoed); vga_puts("B)");
            vga_puts(" Errors: "); vga_put_dec(g_echo.errors); vga_puts("\n");
            vga_puts("  Use 'sma stop' to stop.\n");
        } else {
            uint16_t port = 7;
            if (arg[0] >= '0' && arg[0] <= '9') port = (uint16_t)parse_num(arg);
            net_echo_start(port);
        }
    } else if (strcmp(cmd, "dhcp") == 0) {
        vga_puts_color("=== DHCP Client (dhcp) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
        net_dhcp_request();
    /* === NEW: Kill, Filesystem === */
    } else if (strncmp(cmd, "aqtl", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        cmd_aqtl(cmd + 4);
    } else if (strcmp(cmd, "tnsyq") == 0) {
        cmd_tnsyq();
    } else if (strcmp(cmd, "hfzk") == 0) {
        cmd_hfzk();
    } else if (strcmp(cmd, "hmml") == 0) {
        cmd_hmml();
    } else if (strcmp(cmd, "dskfs") == 0) {
        cmd_dskfs();
    } else if (strcmp(cmd, "khlf") == 0) {
        cmd_khlf();
    } else if (strncmp(cmd, "fzh", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        cmd_fzh(cmd + 3);
    } else if (strcmp(cmd, "ajhz") == 0) {
        cmd_ajhz();
    } else if (strncmp(cmd, "dev", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        cmd_dev(cmd + 3);
    } else if (strncmp(cmd, "anbb", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        cmd_anbb(cmd + 4);
    } else if (strncmp(cmd, "ishara", 6) == 0 && (cmd[6] == ' ' || cmd[6] == '\0')) {
        cmd_ishara(cmd + 6);
    } else if (strcmp(cmd, "thkra") == 0) {
        cmd_sbrk_demo();
    } else if (strcmp(cmd, "wqt") == 0) {
        cmd_time();
    } else if (strncmp(cmd, "mjld", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        cmd_mkdir(cmd + 4);
    } else if (strncmp(cmd, "dkhl", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
        cmd_cd(cmd + 4);
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (strncmp(cmd, "tcp", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        const char* sub = skip_spaces(cmd + 3);
        if (strncmp(sub, "connect", 7) == 0) {
            sub = skip_spaces(sub + 7);
            vga_puts_color("=== TCP Connect ===\n", theme_header, VGA_BLACK);
            ip_addr_t ip; uint16_t port = 80;
            int ib[4]={0,0,0,0}; int bi=0,ci=0;
            while (sub[ci] && sub[ci]!=' ') {
                if (sub[ci]=='.'){bi++;ci++;continue;}
                if (bi<4) ib[bi]=ib[bi]*10+(sub[ci]-'0');
                ci++;
            }
            ip.b[0]=(uint8_t)ib[0];ip.b[1]=(uint8_t)ib[1];
            ip.b[2]=(uint8_t)ib[2];ip.b[3]=(uint8_t)ib[3];
            sub = skip_spaces(sub+ci);
            if (*sub>='0'&&*sub<='9') port=(uint16_t)parse_num(sub);
            vga_puts("  Connecting to ");
            for(int i=0;i<4;i++){vga_put_dec(ip.b[i]);if(i<3)vga_putchar('.');}
            vga_puts(":"); vga_put_dec(port); vga_puts("... ");
            int32_t c = tcp_connect(ip, port);
            if (c >= 0) {
                vga_puts_color("CONNECTED", VGA_LIGHT_GREEN, VGA_BLACK);
                vga_printf(" (conn %d)\n", c);
            } else {
                vga_puts_color("FAILED\n", theme_error, VGA_BLACK);
            }
        } else if (strncmp(sub, "send", 4) == 0) {
            sub = skip_spaces(sub+4);
            int32_t conn=0;
            if(*sub>='0'&&*sub<='9'){conn=(int32_t)parse_num(sub);while(*sub&&*sub!=' ')sub++;sub=skip_spaces(sub);}
            if (!*sub) vga_puts("Usage: tcp send [conn] <data>\n");
            else { int32_t r=tcp_send(conn,sub,strlen(sub)); vga_printf("  Sent %d bytes\n",r); }
        } else if (strncmp(sub, "recv", 4) == 0) {
            sub = skip_spaces(sub+4);
            int32_t conn=0;
            if(*sub>='0'&&*sub<='9') conn=(int32_t)parse_num(sub);
            char buf[1024]; memset(buf,0,sizeof(buf));
            int32_t r=tcp_recv(conn,buf,sizeof(buf)-1);
            if(r>0){vga_printf("  Received %d bytes:\n",r);vga_puts(buf);vga_puts("\n");}
            else vga_puts("  No data received.\n");
        } else if (strncmp(sub, "close", 5) == 0) {
            sub = skip_spaces(sub+5);
            int32_t conn=0;
            if(*sub>='0'&&*sub<='9') conn=(int32_t)parse_num(sub);
            tcp_close(conn);
            vga_puts("  Connection closed.\n");
        } else if (strcmp(sub, "list") == 0 || !*sub) {
            tcp_dump();
        } else {
            vga_puts("  tcp connect <ip> <port>\n  tcp send [c] <data>\n  tcp recv [c]\n  tcp close [c]\n  tcp list\n");
        }
    } else if (strncmp(cmd, "dns", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
        const char* host = skip_spaces(cmd+3);
        if (!*host) vga_puts("Usage: dns <hostname>  (e.g. dns google.com)\n");
        else dns_dump(host);
    } else if (strcmp(cmd, "intdhr") == 0) {
        vga_puts_color("=== Waitpid Demo (intdhr) ===\n", theme_header, VGA_BLACK);
        vga_puts("  Creating child process...\n");
        int32_t cpid = proc_create("child_test", proc_exit, 128);
        if (cpid < 0) {
            vga_puts_color("  Failed to create child.\n", theme_error, VGA_BLACK);
        } else {
            vga_printf("  Child PID %u (parent %u)\n", (uint32_t)cpid, current_process->pid);
            vga_puts("  Waiting for child... ");
            int32_t status = 0;
            int32_t r = proc_waitpid((uint32_t)cpid, &status);
            if (r == 0) {
                vga_puts_color("DONE", VGA_LIGHT_GREEN, VGA_BLACK);
                vga_printf(" exit=%d\n", status);
            } else {
                vga_printf("  Wait failed: %d\n", r);
            }
        }
    } else {
        vga_puts_color("Unknown command: ", theme_error, VGA_BLACK);
        vga_puts(cmd);
        vga_puts("\n");
        vga_puts_color("Type 'sd' for help.\n", VGA_DARK_GREY, VGA_BLACK);
    }
}

/* === Shell Entry Points === */

void shell_init(void) {
    cmd_len = 0;
    hist_count = 0;
    hist_write = 0;
    hist_browse = -1;
    mouse_prev_x = -1;
    mouse_prev_y = -1;
    memset(history, 0, sizeof(history));
}

void shell_run(void) {
    /* Welcome */
    vga_puts("\n");
    vga_puts_color("  Welcome to ", VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts_color("AIOS Shell", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts_color(" - Arabic-Inspired OS Interface\n", VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts_color("  Type 'sd' (saed/help) for available commands.\n\n", VGA_DARK_GREY, VGA_BLACK);

    for (;;) {
        shell_prompt();
        shell_readline();

        const char* line = skip_spaces(cmd_buf);
        if (!line[0]) continue;
        history_add(cmd_buf);

        /* Check for pipe: cmd1 | cmd2 */
        const char* pipe_pos = NULL;
        for (const char* p = line; *p; p++) {
            if (*p == '|') { pipe_pos = p; break; }
        }

        if (pipe_pos) {
            /* Split at pipe */
            char cmd1[SHELL_CMD_MAX];
            char cmd2[SHELL_CMD_MAX];
            uint32_t len1 = (uint32_t)(pipe_pos - line);
            if (len1 >= SHELL_CMD_MAX) len1 = SHELL_CMD_MAX - 1;
            memcpy(cmd1, line, len1);
            cmd1[len1] = '\0';
            /* Trim trailing spaces from cmd1 */
            while (len1 > 0 && cmd1[len1-1] == ' ') cmd1[--len1] = '\0';

            const char* c2 = skip_spaces(pipe_pos + 1);
            uint32_t len2 = strlen(c2);
            if (len2 >= SHELL_CMD_MAX) len2 = SHELL_CMD_MAX - 1;
            memcpy(cmd2, c2, len2);
            cmd2[len2] = '\0';

            /* Capture output of cmd1 */
            static char pipe_buf[4096];
            vga_capture_start(pipe_buf, sizeof(pipe_buf));
            shell_exec(cmd1);
            uint32_t cap_len = vga_capture_stop();

            /* Pass captured output to cmd2 by writing to temp file */
            if (cap_len > 0) {
                vfs_delete("_pipe_tmp");
                vfs_create("_pipe_tmp", pipe_buf, cap_len);
            }

            /* Run cmd2 — if it's 'rq _pipe_tmp', it shows piped output
             * For other commands, pipe output is available in _pipe_tmp */
            vga_puts_color("[pipe] ", VGA_DARK_GREY, VGA_BLACK);
            vga_put_dec(cap_len);
            vga_puts_color(" bytes piped\n", VGA_DARK_GREY, VGA_BLACK);

            /* Special: "rq" without args on pipe = show pipe content */
            if (strcmp(cmd2, "rq") == 0) {
                vga_puts(pipe_buf);
            } else {
                shell_exec(cmd2);
            }
            vfs_delete("_pipe_tmp");
        } else {
            shell_exec(line);
        }
    }
}
