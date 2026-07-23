/* =============================================================================
 * libaios — fixed-width types for Ring 3 user programs
 *
 * User programs are built with -nostdinc: there is no <stdint.h>, no <stddef.h>
 * and no libc of any kind. This header is the whole type vocabulary, and it is
 * deliberately i386-specific (ILP32) — the only target AIOS user mode runs on.
 *
 * This file must NOT be shared with the kernel. The kernel has its own
 * include/kernel/types.h; keeping the two apart is what lets user code be
 * compiled with a completely separate include path (-Isrc/user/lib) so it can
 * never accidentally pull in kernel internals.
 * ========================================================================== */
#ifndef _LIBAIOS_TYPES_H
#define _LIBAIOS_TYPES_H

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

typedef uint32_t           size_t;   /* 32-bit target: size_t is 32 bits */
typedef int32_t            ssize_t;

#define NULL ((void*)0)

typedef enum { false = 0, true = 1 } bool;

#endif /* _LIBAIOS_TYPES_H */
