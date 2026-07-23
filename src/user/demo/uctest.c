/* =============================================================================
 * uctest — libaios demonstration / self-test, running in Ring 3
 *
 * Exercises every layer of the user library and reports each step as [ok] or
 * [FAIL], finishing with a pass/fail tally. Built and embedded exactly like
 * hello.elf; run it from the shell with:  shgl uctest.elf
 *
 * Stack discipline: the user stack is ONE 4KB page (ABI.md). There is not a
 * single large array on the stack anywhere below — every buffer bigger than a
 * number formatter's scratch space comes from malloc.
 * ========================================================================== */

#include "aios.h"

static uint32_t g_pass = 0;
static uint32_t g_fail = 0;

/* Report one checked step. Keeping this in one place is what makes every test
 * below a single readable line. */
static void check(int ok, const char* what) {
    if (ok) { g_pass++; print("  [ok]   "); }
    else    { g_fail++; print("  [FAIL] "); }
    print(what);
    print("\n");
}

/* ---------------------------------------------------------------- strings -- */
static void test_strings(void) {
    print("\n-- strings --\n");

    check(strlen("") == 0 && strlen("abc") == 3, "strlen");
    check(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0 &&
          strcmp("b", "a") > 0, "strcmp");
    check(strncmp("abcdef", "abcXXX", 3) == 0 &&
          strncmp("abcdef", "abcXXX", 4) != 0, "strncmp");

    char buf[16];
    size_t n = strlcpy(buf, "hello", sizeof(buf));
    check(n == 5 && strcmp(buf, "hello") == 0, "strlcpy (fits)");

    /* Truncation must still NUL-terminate, and the return value must report
     * the length that WOULD have been needed. */
    n = strlcpy(buf, "0123456789ABCDEFGHIJ", 8);
    check(n == 20 && strlen(buf) == 7 && strcmp(buf, "0123456") == 0,
          "strlcpy (truncates, still terminated)");

    memset(buf, 'z', 8);
    check(buf[0] == 'z' && buf[7] == 'z', "memset");

    memcpy(buf, "wxyz", 4);
    check(memcmp(buf, "wxyz", 4) == 0 && memcmp(buf, "wxya", 4) != 0,
          "memcpy + memcmp");
}

/* ---------------------------------------------------------------- numbers -- */
static void test_numbers(void) {
    print("\n-- number formatting --\n");

    char buf[16];

    utoa(0, buf, sizeof(buf), 10);
    check(strcmp(buf, "0") == 0, "utoa 0");

    utoa(4294967295u, buf, sizeof(buf), 10);
    check(strcmp(buf, "4294967295") == 0, "utoa UINT32_MAX (base 10)");

    utoa(0xDEADBEEFu, buf, sizeof(buf), 16);
    check(strcmp(buf, "deadbeef") == 0, "utoa 0xDEADBEEF (base 16)");

    itoa(-12345, buf, sizeof(buf), 10);
    check(strcmp(buf, "-12345") == 0, "itoa negative");

    /* INT32_MIN has no positive counterpart; the unsigned negation trick in
     * itoa() is what keeps this from overflowing. */
    itoa((int32_t)0x80000000, buf, sizeof(buf), 10);
    check(strcmp(buf, "-2147483648") == 0, "itoa INT32_MIN");

    print("  sample output: dec=");
    print_dec(-42);
    print(" udec=");
    print_udec(3735928559u);
    print(" hex=");
    print_hex(0xCAFEBABEu);
    print("\n");
}

/* ----------------------------------------------------------------- malloc -- */
static void test_malloc(void) {
    print("\n-- malloc / free --\n");

    /* Three blocks of different sizes, each filled with a distinct pattern so
     * an overlap or a stale pointer shows up as a verification failure. */
    uint8_t* a = (uint8_t*)malloc(64);
    uint8_t* b = (uint8_t*)malloc(1024);
    uint8_t* c = (uint8_t*)malloc(200);
    check(a && b && c, "malloc x3 returned non-NULL");

    if (!a || !b || !c) return;

    check(((uint32_t)a % 8) == 0 && ((uint32_t)b % 8) == 0 &&
          ((uint32_t)c % 8) == 0, "blocks are 8-byte aligned");

    memset(a, 0xA1, 64);
    memset(b, 0xB2, 1024);
    memset(c, 0xC3, 200);

    int ok = 1;
    for (size_t i = 0; i < 64;   i++) if (a[i] != 0xA1) ok = 0;
    for (size_t i = 0; i < 1024; i++) if (b[i] != 0xB2) ok = 0;
    for (size_t i = 0; i < 200;  i++) if (c[i] != 0xC3) ok = 0;
    check(ok, "patterns verify (no overlap between blocks)");

    /* Free the middle block and re-request the same size: a first-fit
     * allocator must hand the very same address back. */
    free(b);
    uint8_t* b2 = (uint8_t*)malloc(1024);
    check(b2 == b, "freed block is reused for an identical request");

    free(a);
    free(c);
    free(b2);

    /* Everything is free and adjacent, so coalescing should have merged the
     * whole heap back into a small number of blocks. */
    malloc_stats_t st;
    malloc_stats(&st);
    check(st.in_use_bytes == 0, "all blocks accounted for after free");

    print("  heap: ");
    print_udec(st.heap_bytes);
    print(" bytes from sbrk in ");
    print_udec(st.sbrk_calls);
    print(" call(s), ");
    print_udec(st.blocks);
    print(" block(s) after coalescing\n");

    /* free(NULL) and a bogus pointer must both be silently survivable. */
    free(NULL);
    uint32_t junk = 0;
    free(&junk);
    check(1, "free(NULL) and free(bad pointer) survived");
}

static void test_realloc(void) {
    print("\n-- realloc / calloc --\n");

    char* p = (char*)malloc(16);
    if (!p) { check(0, "realloc setup"); return; }
    strlcpy(p, "preserve me", 16);

    char* q = (char*)realloc(p, 256);
    check(q != NULL && strcmp(q, "preserve me") == 0,
          "realloc grows and preserves contents");

    char* r = (char*)realloc(q, 8);
    check(r != NULL && strncmp(r, "preserve", 8) == 0, "realloc shrinks");
    free(r);

    uint8_t* z = (uint8_t*)calloc(128, 1);
    int zeroed = (z != NULL);
    if (z) for (size_t i = 0; i < 128; i++) if (z[i]) zeroed = 0;
    check(zeroed, "calloc returns zeroed memory");
    free(z);

    /* nmemb * size must not be allowed to wrap. */
    check(calloc(0x10000000u, 64) == NULL, "calloc rejects overflowing size");
}

/* ------------------------------------------------------------------- file -- */
static void test_file(void) {
    print("\n-- file I/O (read-only) --\n");

    int32_t fd = sys_open("readme.txt");
    check(fd >= 3, "sys_open(\"readme.txt\") returned a user fd");
    if (fd < 0) return;

    /* Read buffer comes from the heap, not the stack — the stack is one page. */
    char* buf = (char*)malloc(64);
    if (!buf) { check(0, "buffer allocation"); sys_fclose(fd); return; }

    int32_t n = sys_fread(fd, buf, 32);
    check(n == 32, "sys_fread returned 32 bytes");

    if (n > 0) {
        buf[n] = '\0';
        check(strncmp(buf, "AIOS", 4) == 0, "file contents start with \"AIOS\"");
        print("  first 32 bytes: \"");
        print_n(buf, (size_t)n);
        print("\"\n");
    }

    /* Reading on continues from the current offset — the descriptor carries
     * its own position even though the ABI exposes no seek. */
    int32_t n2 = sys_fread(fd, buf, 16);
    check(n2 > 0, "second sys_fread continues from the offset");

    check(sys_fclose(fd) == 0, "sys_fclose succeeded");

    /* Every one of these must be rejected, not honoured. */
    check(sys_fread(fd, buf, 8) < 0, "read on a closed fd is rejected");
    check(sys_fread(1, buf, 8) < 0, "read on stdout (fd 1) is rejected");
    check(sys_open("no_such_file.txt") < 0, "open of a missing file fails");

    free(buf);
}

/* --------------------------------------------------------------- syscalls -- */
static void test_syscalls(void) {
    print("\n-- assorted syscalls --\n");

    uint32_t pid = sys_getpid();
    check(pid > 0, "sys_getpid returned a real pid");
    print("  pid=");
    print_udec(pid);

    print(" uptime=");
    print_udec(sys_uptime());
    print("s\n");

    sys_stats_t* st = (sys_stats_t*)malloc(sizeof(sys_stats_t));
    if (st) {
        check(sys_getstats(st) == 0, "sys_getstats filled the struct");
        print("  free_pages=");
        print_udec(st->free_pages);
        print(" syscalls=");
        print_udec(st->total_syscalls);
        print("\n");
        free(st);
    }

    sys_yield();
    check(1, "sys_yield returned (still scheduled)");
}

/* ----------------------------------------------------------- heap cap ----- */
/* Allocate until the allocator reports exhaustion. The point is not to consume
 * memory but to prove the failure is GRACEFUL: sbrk hits USER_HEAP_MAX,
 * returns (void*)-1, malloc turns that into NULL, and we keep running. If this
 * were mishandled the process would fault into the guard page and be killed,
 * and none of the lines after this would ever print.
 *
 * This runs last because it leaves the heap full. */
static void test_heap_cap(void) {
    print("\n-- heap cap (graceful exhaustion) --\n");

    uint32_t blocks = 0;
    for (uint32_t i = 0; i < 100000u; i++) {
        void* p = malloc(4096);
        if (!p) break;
        blocks++;
    }

    print("  malloc(4096) succeeded ");
    print_udec(blocks);
    print(" times, then returned NULL\n");

    check(blocks > 0, "allocations succeeded before the cap");
    check(malloc(4096) == NULL, "malloc returns NULL at the heap cap");
    check(malloc(1) == NULL || 1, "still running after exhaustion (no fault)");

    malloc_stats_t st;
    malloc_stats(&st);
    print("  heap at cap: ");
    print_udec(st.heap_bytes);
    print(" bytes over ");
    print_udec(st.sbrk_calls);
    print(" sbrk call(s)\n");
}

/* ------------------------------------------------------------------- main -- */
int main(void) {
    print("\n");
    print("  ==============================================\n");
    print("  |  uctest - libaios user-space runtime test  |\n");
    print("  |  Ring 3, no libc, syscalls via INT 0x80    |\n");
    print("  ==============================================\n");

    test_strings();
    test_numbers();
    test_malloc();
    test_realloc();
    test_file();
    test_syscalls();
    test_heap_cap();          /* must be last: it fills the heap */

    print("\n  ----------------------------------------------\n");
    print("  RESULT: ");
    print_udec(g_pass);
    print(" passed, ");
    print_udec(g_fail);
    print(" failed");
    print(g_fail == 0 ? "  <<< ALL OK >>>\n" : "  <<< FAILURES >>>\n");
    print("  ----------------------------------------------\n");

    /* crt0 issues SYS_EXIT; ABI v1 carries no exit status, so this value is
     * discarded. It is here for readability and for a future ABI that has one. */
    return g_fail == 0 ? 0 : 1;
}
