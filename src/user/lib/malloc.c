/* =============================================================================
 * libaios — heap allocator (implementation)
 *
 * Design: a single address-ordered list of ALL blocks, free and in use, each
 * preceded by a header. First-fit search, split on allocate, coalesce with the
 * following block on free.
 *
 * Why one list of everything rather than a separate free list: with the blocks
 * kept in address order, coalescing is just "is my successor free and
 * adjacent?" — no sorting, no boundary tags, no second data structure to keep
 * consistent. The heap is contiguous by construction (sbrk only ever extends
 * upward), so address order and list order are the same thing.
 *
 * The cost is that malloc walks in-use blocks too. For the workloads this
 * substrate is meant to carry that is a fine trade for being obviously
 * correct; a size-bucketed free list can replace the search later without
 * changing the block layout or the public API.
 * ========================================================================== */

#include "malloc.h"
#include "syscalls.h"
#include "string.h"

/* Payload alignment. 8 keeps uint64_t members naturally aligned. */
#define ALIGN        8u

/* Minimum bytes to request from the kernel at a time. Each sbrk call is a
 * syscall and rounds up to whole 4KB pages kernel-side, so batching keeps both
 * the trap count and the page-table churn down. */
#define CHUNK_BYTES  8192u

#define BLOCK_MAGIC  0xA105B10Cu   /* "AIOS BLOC" */

typedef struct block {
    uint32_t      magic;   /* BLOCK_MAGIC — catches free() of a bad pointer */
    uint32_t      size;    /* payload bytes, always a multiple of ALIGN     */
    struct block* next;    /* next block by ADDRESS, NULL at the top        */
    uint32_t      free;    /* 1 = on the free list, 0 = handed out          */
} block_t;

/* sizeof(block_t) is 16 on i386, itself a multiple of ALIGN, so a header
 * placed at an aligned address leaves the payload aligned too. */
#define HDR_SIZE (sizeof(block_t))

static block_t* g_head = NULL;   /* lowest-addressed block, NULL until first sbrk */
static block_t* g_tail = NULL;   /* highest-addressed block — where growth lands  */
static uint32_t g_heap_bytes = 0;
static uint32_t g_sbrk_calls = 0;

static size_t align_up(size_t n) {
    return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

static void* payload_of(block_t* b) {
    return (void*)((uint8_t*)b + HDR_SIZE);
}

static block_t* block_of(void* p) {
    return (block_t*)((uint8_t*)p - HDR_SIZE);
}

/* Ask the kernel for at least `need` more payload bytes and append the result
 * as one new free block. Returns that block, or NULL at the heap cap.
 *
 * The new region is always immediately above the previous break, so if the
 * current top block is free we merge into it instead of leaving a seam. */
static block_t* heap_grow(size_t need) {
    size_t want = align_up(need + HDR_SIZE);
    if (want < CHUNK_BYTES) want = CHUNK_BYTES;

    void* base = sys_sbrk((int32_t)want);
    /* ABI.md: sbrk reports failure as (void*)-1, NOT as NULL. At the heap cap
     * (USER_HEAP_MAX) or if the kernel is out of frames, this is the path. */
    if (base == (void*)-1) return NULL;

    g_sbrk_calls++;
    g_heap_bytes += want;

    /* Merge into the top block if it is free — sbrk hands back contiguous
     * memory, so the two are physically adjacent. */
    if (g_tail && g_tail->free &&
        (uint8_t*)payload_of(g_tail) + g_tail->size == (uint8_t*)base) {
        g_tail->size += want;
        return g_tail;
    }

    block_t* b = (block_t*)base;
    b->magic = BLOCK_MAGIC;
    b->size  = want - HDR_SIZE;
    b->next  = NULL;
    b->free  = 1;

    if (g_tail) g_tail->next = b;
    else        g_head = b;
    g_tail = b;
    return b;
}

/* Split `b` so it holds exactly `size` payload bytes, provided the remainder
 * can carry a header plus a useful payload. Otherwise leave it whole (the few
 * slack bytes stay with the block). */
static void block_split(block_t* b, size_t size) {
    if (b->size < size + HDR_SIZE + ALIGN) return;

    block_t* rest = (block_t*)((uint8_t*)payload_of(b) + size);
    rest->magic = BLOCK_MAGIC;
    rest->size  = b->size - size - HDR_SIZE;
    rest->next  = b->next;
    rest->free  = 1;

    b->size = size;
    b->next = rest;
    if (g_tail == b) g_tail = rest;
}

/* Merge `b` with its successor while that successor is free and adjacent. */
static void block_coalesce(block_t* b) {
    while (b->next && b->next->free &&
           (uint8_t*)payload_of(b) + b->size == (uint8_t*)b->next) {
        block_t* n = b->next;
        b->size += HDR_SIZE + n->size;
        b->next  = n->next;
        if (g_tail == n) g_tail = b;
        n->magic = 0;                 /* poison the absorbed header */
    }
}

void* malloc(size_t size) {
    if (size == 0) return NULL;
    size = align_up(size);

    /* Refuse anything that could not possibly fit, and guard the arithmetic in
     * heap_grow against wrapping on an absurd request. */
    if (size > 0x40000000u) return NULL;

    for (block_t* b = g_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            block_split(b, size);
            b->free = 0;
            return payload_of(b);
        }
    }

    block_t* b = heap_grow(size);
    if (!b) return NULL;              /* heap cap reached — caller sees NULL */
    block_split(b, size);
    b->free = 0;
    return payload_of(b);
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb && size > 0xFFFFFFFFu / nmemb) return NULL;   /* overflow */
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void free(void* ptr) {
    if (!ptr) return;

    block_t* b = block_of(ptr);
    if (b->magic != BLOCK_MAGIC || b->free) return;   /* not ours, or double free */

    b->free = 1;
    block_coalesce(b);

    /* Coalesce backwards too. The list is singly linked, so find the
     * predecessor by walking from the head — O(n), but free() is not on any
     * hot path and this keeps the block layout free of boundary tags. */
    for (block_t* p = g_head; p && p != b; p = p->next) {
        if (p->next == b && p->free) { block_coalesce(p); break; }
    }
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    block_t* b = block_of(ptr);
    if (b->magic != BLOCK_MAGIC) return NULL;

    size_t want = align_up(size);
    if (b->size >= want) {                /* shrinking, or already big enough */
        block_split(b, want);
        return ptr;
    }

    /* Try to absorb a free, adjacent successor before falling back to a copy. */
    block_coalesce(b);
    if (b->size >= want) {
        block_split(b, want);
        return ptr;
    }

    void* n = malloc(size);
    if (!n) return NULL;                  /* original block left untouched */
    memcpy(n, ptr, b->size);
    free(ptr);
    return n;
}

void malloc_stats(malloc_stats_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->heap_bytes = g_heap_bytes;
    out->sbrk_calls = g_sbrk_calls;
    for (block_t* b = g_head; b; b = b->next) {
        out->blocks++;
        if (b->free) out->free_bytes += b->size;
        else         out->in_use_bytes += b->size;
    }
}
