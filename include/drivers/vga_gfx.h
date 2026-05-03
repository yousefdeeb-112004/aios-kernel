/* =============================================================================
 * VGA Visual Dashboard — Text-Mode Graphics
 *
 * Uses colored text cells (80x25, 16 colors) to create a visual
 * dashboard without switching VGA modes. 100% reliable.
 *
 * Features:
 *   - Colored block drawing using space characters with backgrounds
 *   - Box drawing with border characters
 *   - Horizontal bar charts
 *   - Progress bars
 *   - System dashboard with live stats
 *   - Interactive mouse cursor
 *   - Multiple dashboard pages (key 1-4)
 *
 * Shell command: rsm (رسم rasm = draw)
 * ============================================================================= */
#ifndef _DRIVERS_VGA_GFX_H
#define _DRIVERS_VGA_GFX_H

#include <kernel/types.h>

/* Run the graphics dashboard */
void gfx_demo(void);

#endif
