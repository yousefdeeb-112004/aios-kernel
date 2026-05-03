/* =============================================================================
 * Window Manager — Text-Mode Compositing (Tier 3.4)
 *
 * Shell command: nfdh (نافذة nafdha = window) — launch window manager
 *
 * Features:
 *   - Up to 8 overlapping windows with title bars
 *   - Mouse click to focus/raise, drag title bar to move
 *   - Close [X] button on each window
 *   - Taskbar at bottom showing open windows
 *   - Built-in apps: About, SysMon, Files, Clock, Notepad, NetInfo
 *   - F1-F6 to open apps, ESC to exit
 * ============================================================================= */
#ifndef _KERNEL_WM_H
#define _KERNEL_WM_H

#include <kernel/types.h>

#define WM_MAX_WINDOWS  8
#define WM_SCREEN_W     80
#define WM_SCREEN_H     25

typedef enum {
    WM_APP_ABOUT, WM_APP_SYSMON, WM_APP_FILES,
    WM_APP_CLOCK, WM_APP_NOTEPAD, WM_APP_NETINFO,
} wm_app_t;

typedef struct {
    int x, y, w, h;
    char title[24];
    uint8_t color;      /* Title bar color */
    wm_app_t app;
    bool open;
    /* Notepad buffer */
    char note[200];
    uint32_t note_len;
} wm_window_t;

typedef struct {
    wm_window_t win[WM_MAX_WINDOWS];
    int count;
    int focus;          /* Focused window index, -1=none */
    int drag;           /* Dragging window index, -1=none */
    int drag_ox, drag_oy;
    bool running;
} wm_state_t;

extern wm_state_t g_wm;

void wm_run(void);

#endif
