/* =============================================================================
 * libaios — heap allocator for Ring 3 programs
 *
 * Built entirely on SYS_SBRK. Per ABI.md the user heap runs from
 * USER_HEAP_START (0x00540000) up to USER_HEAP_MAX (0x005FE000) — 760 KB —
 * with a permanently unmapped guard page above it. When the break reaches the
 * cap, sbrk returns (void*)-1; malloc turns that into a clean NULL rather than
 * faulting, so running out of memory is a recoverable condition.
 *
 * Note that ABI v1.1's sbrk does not release pages on a negative increment, so
 * this allocator never shrinks the break: freed memory returns to the free
 * list for reuse, not to the kernel. That is the documented behaviour, not an
 * oversight here.
 * ========================================================================== */
#ifndef _LIBAIOS_MALLOC_H
#define _LIBAIOS_MALLOC_H

#include "types.h"

/* Returns a pointer to at least `size` usable bytes, 8-byte aligned, or NULL
 * if the heap cap is reached. malloc(0) returns NULL. Contents are undefined. */
void* malloc(size_t size);

/* Returns a block of nmemb*size bytes, zeroed, or NULL. Overflow-safe. */
void* calloc(size_t nmemb, size_t size);

/* Releases a block obtained from malloc/calloc/realloc. free(NULL) is a no-op.
 * Passing anything else is a programming error and is ignored (the block
 * header carries a magic number that is checked first). */
void  free(void* ptr);

/* Grows or shrinks a block, preserving its contents up to the smaller of the
 * two sizes. realloc(NULL, n) == malloc(n); realloc(p, 0) frees and returns
 * NULL. Returns NULL and leaves the original block intact on failure. */
void* realloc(void* ptr, size_t size);

/* --- introspection (used by the demo and, later, by the interpreter) ------- */
typedef struct {
    uint32_t heap_bytes;    /* total bytes obtained from sbrk               */
    uint32_t in_use_bytes;  /* payload bytes currently handed out           */
    uint32_t free_bytes;    /* payload bytes on the free list               */
    uint32_t blocks;        /* total blocks (free + in use)                 */
    uint32_t sbrk_calls;    /* how many times we grew the heap              */
} malloc_stats_t;

void malloc_stats(malloc_stats_t* out);

#endif /* _LIBAIOS_MALLOC_H */
