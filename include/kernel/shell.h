/* =============================================================================
 * AIOS Shell — Arabic-Inspired Command Interface
 *
 * Commands (Arabic roots):
 *   db   (عرض)  — display/list files
 *   rq   (اقرأ) — read file content
 *   nz   (نظر)  — system info/look
 *   am   (عمليات) — show processes
 *   zk   (ذاكرة) — memory stats
 *   dk   (ذكاء) — AI intelligence report
 *   ms   (مسح)  — clear screen
 *   sd   (ساعد) — help
 *   wqf  (وقف)  — halt system
 *   ktb  (كتب)  — write/create file
 * ============================================================================= */
#ifndef _KERNEL_SHELL_H
#define _KERNEL_SHELL_H

#include <kernel/types.h>

#define SHELL_CMD_MAX  128   /* Max command line length */
#define SHELL_HISTORY  8     /* Command history slots */

/* Initialize and run the shell (never returns) */
void shell_init(void);
void shell_run(void);

#endif
