#ifndef _DRIVERS_VGA_H
#define _DRIVERS_VGA_H
#include <kernel/types.h>
typedef enum { VGA_BLACK=0,VGA_BLUE=1,VGA_GREEN=2,VGA_CYAN=3,VGA_RED=4,VGA_MAGENTA=5,VGA_BROWN=6,VGA_LIGHT_GREY=7,VGA_DARK_GREY=8,VGA_LIGHT_BLUE=9,VGA_LIGHT_GREEN=10,VGA_LIGHT_CYAN=11,VGA_LIGHT_RED=12,VGA_LIGHT_MAGENTA=13,VGA_YELLOW=14,VGA_WHITE=15 } vga_color_t;
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_BUFFER 0xB8000
void vga_init(void); void vga_clear(void); void vga_set_color(vga_color_t fg, vga_color_t bg);
void vga_putchar(char c); void vga_puts(const char* str);
void vga_puts_color(const char* str, vga_color_t fg, vga_color_t bg);
void vga_put_hex(uint32_t value); void vga_put_dec(uint32_t value);
void vga_set_cursor(int x, int y); void vga_scroll(void);
/* Scroll-back buffer */
void vga_scroll_back(void);    /* Page Up — show older output */
void vga_scroll_forward(void); /* Page Down — show newer output */
void vga_scroll_reset(void);   /* Return to live view */
bool vga_is_scrolled(void);    /* True if viewing scroll-back */

/* Output capture for shell pipes */
void vga_capture_start(char* buf, uint32_t max_len);
uint32_t vga_capture_stop(void);
bool vga_is_capturing(void);
#endif
