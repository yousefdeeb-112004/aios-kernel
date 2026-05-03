/* PS/2 Keyboard Driver — Final Version
 *
 * Handles:
 *   - Normal ASCII keys (scancode set 1)
 *   - Shift modifier
 *   - Ctrl modifier (Ctrl+letter → control character)
 *   - E0 extended scancodes (arrow keys, etc.)
 *
 * Arrow keys (via E0 prefix):
 *   E0 48 = Up    → KEY_UP    (1)
 *   E0 50 = Down  → KEY_DOWN  (2)
 *   E0 4B = Left  → KEY_LEFT  (3)
 *   E0 4D = Right → KEY_RIGHT (4)
 */
#include <drivers/keyboard.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/ports.h>
#include <drivers/vga.h>

keyboard_stats_t g_kb_stats;
static char kb_buf[KB_BUFFER_SIZE];
static uint32_t kb_h = 0, kb_t = 0, kb_c = 0;
static bool shift = false;
static bool ctrl = false;
static bool echo_enabled = true;
static bool e0_pending = false;

/* Scancode set 1: normal keys */
static const char sc_normal[58] = {
    0, 27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
    'z','x','c','v','b','n','m',',','.','/', 0,'*', 0,' '
};

/* Scancode set 1: shifted keys */
static const char sc_shift[58] = {
    0, 27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|',
    'Z','X','C','V','B','N','M','<','>','?', 0,'*', 0,' '
};

static void buf_push(char c) {
    if (kb_c >= KB_BUFFER_SIZE) { g_kb_stats.buffer_overflows++; return; }
    kb_buf[kb_h] = c;
    kb_h = (kb_h + 1) % KB_BUFFER_SIZE;
    kb_c++;
}

static void irq_handler(registers_t* r) {
    (void)r;
    uint8_t code = inb(KB_DATA_PORT);
    g_kb_stats.last_scancode = code;

    /* E0 prefix: next byte is an extended scancode */
    if (code == 0xE0) {
        e0_pending = true;
        pic_send_eoi(1);
        return;
    }

    /* === Extended key (after E0) === */
    if (e0_pending) {
        e0_pending = false;

        /* Extended key release — ignore */
        if (code & 0x80) {
            g_kb_stats.total_releases++;
            pic_send_eoi(1);
            return;
        }

        /* Extended key press — check for arrows + page keys */
        g_kb_stats.total_keypresses++;
        char special = 0;
        switch (code) {
            case 0x48: special = shift ? KEY_PGUP  : KEY_UP;    break;
            case 0x50: special = shift ? KEY_PGDN  : KEY_DOWN;  break;
            case 0x4B: special = KEY_LEFT;  break;
            case 0x4D: special = KEY_RIGHT; break;
            case 0x49: special = KEY_PGUP;  break;
            case 0x51: special = KEY_PGDN;  break;
        }
        if (special) {
            g_kb_stats.last_char = special;
            buf_push(special);
        }
        pic_send_eoi(1);
        return;
    }

    /* === Normal key release === */
    if (code & 0x80) {
        g_kb_stats.total_releases++;
        uint8_t released = code & 0x7F;
        if (released == 0x2A || released == 0x36) shift = false;
        if (released == 0x1D) ctrl = false;
        pic_send_eoi(1);
        return;
    }

    /* === Normal key press === */
    g_kb_stats.total_keypresses++;

    /* Modifier keys — don't generate characters */
    if (code == 0x2A || code == 0x36) { shift = true; pic_send_eoi(1); return; }
    if (code == 0x1D) { ctrl = true; pic_send_eoi(1); return; }

    /* Look up character in scancode table */
    if (code < 58) {
        char c = shift ? sc_shift[code] : sc_normal[code];
        if (c) {
            /* Ctrl held: generate control character */
            if (ctrl) {
                if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
                else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
            }

            g_kb_stats.last_char = c;
            buf_push(c);

            /* Echo only printable characters */
            if (echo_enabled && c >= ' ' && c < 127)
                vga_putchar(c);
        }
    }

    pic_send_eoi(1);
}

void keyboard_init(void) {
    g_kb_stats.total_keypresses = 0;
    g_kb_stats.total_releases = 0;
    g_kb_stats.last_scancode = 0;
    g_kb_stats.last_char = 0;
    g_kb_stats.buffer_overflows = 0;
    kb_h = 0; kb_t = 0; kb_c = 0;
    shift = false; ctrl = false;
    echo_enabled = true;
    e0_pending = false;
    idt_register_handler(33, irq_handler);
    pic_unmask_irq(1);
}

char keyboard_getchar(void) {
    if (kb_c == 0) return 0;
    char c = kb_buf[kb_t];
    kb_t = (kb_t + 1) % KB_BUFFER_SIZE;
    kb_c--;
    return c;
}

bool keyboard_has_char(void) { return kb_c > 0; }
uint32_t keyboard_buffer_count(void) { return kb_c; }
void keyboard_set_echo(bool e) { echo_enabled = e; }
