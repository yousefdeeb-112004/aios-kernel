/* =============================================================================
 * libaios — umbrella header
 *
 * A Ring 3 program only ever needs this one include. Everything reachable from
 * here is user-side: nothing in libaios includes a kernel header, and the
 * build gives user code the include path -Isrc/user/lib and nothing else, so
 * that separation is enforced by the compiler rather than by convention.
 * ========================================================================== */
#ifndef _LIBAIOS_H
#define _LIBAIOS_H

#include "types.h"
#include "syscalls.h"
#include "string.h"
#include "malloc.h"

/* Programs define this instead of _start; crt0.S calls it and then issues
 * SYS_EXIT. The return value is discarded — ABI v1's SYS_EXIT carries no
 * status — so use it for readability only. */
int main(void);

#endif /* _LIBAIOS_H */
