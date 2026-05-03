/* =============================================================================
 * Text Editor — Fullscreen
 *
 * Renders text content directly to VGA buffer.
 * Status bar shows filename, size, line/col, and help.
 * Ctrl+S saves, ESC exits (prompts to save if modified).
 * ============================================================================= */

#include <kernel/editor.h>
#include <kernel/vfs.h>
#include <kernel/ports.h>
#include <drivers/vga.h>
#include <drivers/keyboard.h>
#include <drivers/pit.h>
#include <lib/string.h>

#define SCR_W 80
#define SCR_H 25
#define EDIT_ROWS (SCR_H - 2)  /* Top status + bottom help = 23 edit rows */
#define VRAM ((uint16_t*)0xB8000)

#define C(fg, bg) ((uint8_t)((fg) | ((bg) << 4)))

static char buf[EDITOR_MAX_SIZE];
static uint32_t buf_len;
static uint32_t cursor;       /* Byte position in buf */
static uint32_t scroll_line;  /* First visible line */
static char filename[64];
static bool modified;
static bool running;

/* Saved screen */
static uint16_t saved_screen[SCR_W * SCR_H];

/* Screen helpers */
static void ecell(int x, int y, char ch, uint8_t color) {
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H)
        VRAM[y * SCR_W + x] = (uint16_t)((uint8_t)ch) | ((uint16_t)color << 8);
}

static void etext(int x, int y, const char* s, uint8_t color) {
    while (*s && x < SCR_W) ecell(x++, y, *s++, color);
}

static void efill(int x, int y, int w, int h, char ch, uint8_t color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            ecell(x + i, y + j, ch, color);
}

static void enum_text(int x, int y, uint32_t v, uint8_t color) {
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; } }
    char r[12]; for (int j = 0; j < i; j++) r[j] = b[i-1-j]; r[i] = 0;
    etext(x, y, r, color);
}

/* Count lines in buffer */
static uint32_t count_lines(void) {
    uint32_t lines = 1;
    for (uint32_t i = 0; i < buf_len; i++)
        if (buf[i] == '\n') lines++;
    return lines;
}

/* Get line number and column from cursor position */
static void cursor_to_linecol(uint32_t pos, uint32_t* line, uint32_t* col) {
    *line = 0; *col = 0;
    for (uint32_t i = 0; i < pos && i < buf_len; i++) {
        if (buf[i] == '\n') { (*line)++; *col = 0; }
        else (*col)++;
    }
}

/* Get buffer offset of start of line N */
static uint32_t line_start(uint32_t line_num) {
    uint32_t ln = 0, pos = 0;
    while (pos < buf_len && ln < line_num) {
        if (buf[pos] == '\n') ln++;
        pos++;
    }
    return pos;
}

/* Insert character at cursor position */
static void insert_char(char c) {
    if (buf_len >= EDITOR_MAX_SIZE - 1) return;
    /* Shift everything after cursor right by 1 */
    for (uint32_t i = buf_len; i > cursor; i--)
        buf[i] = buf[i - 1];
    buf[cursor] = c;
    buf_len++;
    cursor++;
    modified = true;
}

/* Delete character before cursor (backspace) */
static void delete_char(void) {
    if (cursor == 0) return;
    cursor--;
    for (uint32_t i = cursor; i < buf_len - 1; i++)
        buf[i] = buf[i + 1];
    buf_len--;
    modified = true;
}

/* Save file to VFS */
static bool save_file(void) {
    /* Check if file exists */
    if (vfs_exists(filename)) {
        /* Truncate and rewrite */
        vfs_truncate(filename);
        int32_t fd = vfs_open_mode(filename, VFS_MODE_WRITE);
        if (fd < 0) return false;
        vfs_write(fd, buf, buf_len);
        vfs_close(fd);
    } else {
        /* Create new */
        vfs_create(filename, buf, buf_len);
    }
    modified = false;
    return true;
}

/* Render the editor screen */
static void render(void) {
    uint32_t cur_line, cur_col;
    cursor_to_linecol(cursor, &cur_line, &cur_col);

    /* Auto-scroll */
    if (cur_line < scroll_line) scroll_line = cur_line;
    if (cur_line >= scroll_line + EDIT_ROWS) scroll_line = cur_line - EDIT_ROWS + 1;

    /* Top status bar */
    efill(0, 0, SCR_W, 1, ' ', C(15, 1));
    etext(1, 0, "AIOS Editor", C(14, 1));
    etext(14, 0, filename, C(15, 1));
    if (modified) etext(14 + strlen(filename), 0, " [*]", C(12, 1));
    /* Size */
    etext(50, 0, "Size:", C(7, 1));
    enum_text(56, 0, buf_len, C(14, 1));
    etext(62, 0, "Ln:", C(7, 1));
    enum_text(66, 0, cur_line + 1, C(14, 1));
    etext(70, 0, "Col:", C(7, 1));
    enum_text(75, 0, cur_col + 1, C(14, 1));

    /* Edit area */
    efill(0, 1, SCR_W, EDIT_ROWS, ' ', C(7, 0));

    /* Render visible lines */
    uint32_t pos = line_start(scroll_line);
    for (int row = 0; row < EDIT_ROWS; row++) {
        uint32_t ln = scroll_line + row;

        /* Line number gutter */
        uint8_t gutter_c = C(8, 0);
        if (ln == cur_line) gutter_c = C(14, 0);
        int nw = 0;
        uint32_t lv = ln + 1;
        char nb[6]; int ni = 0;
        if (lv == 0) nb[ni++] = '0';
        else { while (lv > 0) { nb[ni++] = '0' + (lv % 10); lv /= 10; } }
        char nr[6]; for (int j = 0; j < ni; j++) nr[j] = nb[ni-1-j]; nr[ni] = 0;
        nw = ni;
        etext(3 - nw, 1 + row, nr, gutter_c);
        ecell(4, 1 + row, 179, C(8, 0));  /* │ separator */

        /* Line content */
        int col = 0;
        while (pos < buf_len && buf[pos] != '\n') {
            uint8_t cc = C(7, 0);
            if (pos == cursor) cc = C(15, 2);  /* Cursor highlight */
            if (col + 5 < SCR_W)
                ecell(5 + col, 1 + row, buf[pos], cc);
            col++;
            pos++;
        }

        /* Show cursor at end of line or on empty line */
        if (pos == cursor && col + 5 < SCR_W) {
            ecell(5 + col, 1 + row, ' ', C(15, 2));
        }

        /* Skip the newline */
        if (pos < buf_len && buf[pos] == '\n') pos++;

        /* If we've gone past end of file, stop */
        if (pos >= buf_len && col == 0 && pos > cursor) break;
    }

    /* Cursor at very end of file (after last char) */
    if (cursor == buf_len) {
        uint32_t end_line, end_col;
        cursor_to_linecol(cursor, &end_line, &end_col);
        int draw_row = (int)(end_line - scroll_line);
        if (draw_row >= 0 && draw_row < EDIT_ROWS && end_col + 5 < (uint32_t)SCR_W)
            ecell(5 + end_col, 1 + draw_row, ' ', C(15, 2));
    }

    /* Bottom help bar */
    efill(0, SCR_H - 1, SCR_W, 1, ' ', C(0, 3));
    etext(1, SCR_H - 1, "ESC:Exit", C(0, 3));
    etext(11, SCR_H - 1, "Ctrl+S:Save", C(0, 3));
    uint32_t total = count_lines();
    etext(24, SCR_H - 1, "Lines:", C(0, 3));
    enum_text(31, SCR_H - 1, total, C(0, 3));
    etext(40, SCR_H - 1, "Max:4KB", C(8, 3));
    if (modified)
        etext(55, SCR_H - 1, "MODIFIED", C(4, 3));
    else
        etext(55, SCR_H - 1, "Saved", C(0, 3));
}

/* Main editor function */
void editor_open(const char* fname) {
    /* Save screen */
    memcpy(saved_screen, VRAM, sizeof(saved_screen));

    strcpy(filename, fname);
    memset(buf, 0, sizeof(buf));
    buf_len = 0;
    cursor = 0;
    scroll_line = 0;
    modified = false;
    running = true;

    /* Load file content if it exists */
    if (vfs_exists(fname)) {
        const vfs_file_t* fi = vfs_info(fname);
        if (fi && fi->readonly) {
            /* Read-only file: allow viewing but not saving */
        }
        int32_t fd = vfs_open(fname);
        if (fd >= 0) {
            int32_t n = vfs_read(fd, buf, EDITOR_MAX_SIZE - 1);
            if (n > 0) buf_len = n;
            vfs_close(fd);
        }
    } else {
        /* New file — will be created on save */
        modified = true;  /* Mark as new */
    }

    keyboard_set_echo(false);

    /* Hide hardware cursor */
    outb(0x3D4, 0x0A); outb(0x3D5, 0x20);

    while (running) {
        render();

        /* Wait for input */
        while (!keyboard_has_char())
            __asm__ volatile("hlt");

        char c = keyboard_getchar();

        /* ESC = exit */
        if (c == 27) {
            if (modified) {
                /* Show save prompt */
                efill(20, 11, 40, 3, ' ', C(15, 4));
                etext(22, 12, "Save before exit? (y/n/c)", C(15, 4));
                while (1) {
                    while (!keyboard_has_char()) __asm__ volatile("hlt");
                    char ans = keyboard_getchar();
                    if (ans == 'y' || ans == 'Y') { save_file(); running = false; break; }
                    if (ans == 'n' || ans == 'N') { running = false; break; }
                    if (ans == 'c' || ans == 'C' || ans == 27) break; /* Cancel */
                }
            } else {
                running = false;
            }
            continue;
        }

        /* Ctrl+S = save (Ctrl+S sends char 19) */
        if (c == 19) {
            const vfs_file_t* fi = vfs_info(filename);
            if (fi && fi->readonly) {
                efill(20, 11, 40, 3, ' ', C(15, 4));
                etext(22, 12, "File is read-only!", C(15, 4));
                pit_sleep_ms(1000);
            } else {
                save_file();
                efill(20, 11, 40, 3, ' ', C(15, 2));
                etext(25, 12, "Saved!", C(15, 2));
                pit_sleep_ms(500);
            }
            continue;
        }

        /* Arrow keys (or Ctrl+P/N as backup) */
        if (c == KEY_UP || c == 16) {
            uint32_t ln, col;
            cursor_to_linecol(cursor, &ln, &col);
            if (ln > 0) {
                uint32_t prev_start = line_start(ln - 1);
                uint32_t prev_end = line_start(ln) - 1;
                uint32_t prev_len = prev_end - prev_start;
                cursor = prev_start + (col < prev_len ? col : prev_len);
            }
            continue;
        }
        if (c == KEY_DOWN || c == 14) {
            uint32_t ln, col;
            cursor_to_linecol(cursor, &ln, &col);
            uint32_t total = count_lines();
            if (ln + 1 < total) {
                uint32_t next_start = line_start(ln + 1);
                uint32_t next_end = buf_len;
                for (uint32_t i = next_start; i < buf_len; i++) {
                    if (buf[i] == '\n') { next_end = i; break; }
                }
                uint32_t next_len = next_end - next_start;
                cursor = next_start + (col < next_len ? col : next_len);
            }
            continue;
        }
        if (c == KEY_LEFT || c == 2) {
            if (cursor > 0) cursor--;
            continue;
        }
        if (c == KEY_RIGHT || c == 6) {
            if (cursor < buf_len) cursor++;
            continue;
        }

        /* Backspace */
        if (c == '\b') {
            delete_char();
            continue;
        }

        /* Enter */
        if (c == '\n') {
            insert_char('\n');
            continue;
        }

        /* Tab = 4 spaces */
        if (c == '\t') {
            for (int i = 0; i < 4; i++) insert_char(' ');
            continue;
        }

        /* Printable characters */
        if (c >= ' ' && c < 127) {
            insert_char(c);
        }
    }

    keyboard_set_echo(true);

    /* Restore hardware cursor */
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);

    /* Restore screen */
    memcpy(VRAM, saved_screen, sizeof(saved_screen));
    vga_init();
}
