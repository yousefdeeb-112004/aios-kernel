/* =============================================================================
 * Text Editor — Fullscreen File Editing
 *
 * A simple but functional text editor that runs fullscreen in the VGA
 * text buffer. Supports typing, backspace, enter, cursor movement,
 * and saves back to VFS on exit.
 *
 * Shell command: thrr (تحرير tahrir = edit)
 * ============================================================================= */
#ifndef _KERNEL_EDITOR_H
#define _KERNEL_EDITOR_H

#include <kernel/types.h>

#define EDITOR_MAX_SIZE 3800  /* Max file content (~95 lines of 40 chars) */

/* Open file in editor. Creates file if it doesn't exist. */
void editor_open(const char* filename);

#endif
