#ifndef _DRIVERS_KEYBOARD_H
#define _DRIVERS_KEYBOARD_H
#include <kernel/types.h>

#define KB_DATA_PORT   0x60
#define KB_BUFFER_SIZE 256

typedef struct {
    uint32_t total_keypresses;
    uint32_t total_releases;
    uint32_t last_scancode;
    char     last_char;
    uint32_t buffer_overflows;
} keyboard_stats_t;

extern keyboard_stats_t g_kb_stats;

void keyboard_init(void);
char keyboard_getchar(void);
bool keyboard_has_char(void);
uint32_t keyboard_buffer_count(void);
void keyboard_set_echo(bool enabled);

/* Special key codes (values 1-4, safe in signed char, won't print as text) */
#define KEY_UP      1
#define KEY_DOWN    2
#define KEY_LEFT    3
#define KEY_RIGHT   4
#define KEY_CTRL_S  19  /* Ctrl+S */
#define KEY_PGUP    5   /* Page Up */
#define KEY_PGDN    6   /* Page Down */

#endif
