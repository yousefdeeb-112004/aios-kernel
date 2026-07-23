/* =============================================================================
 * nawa — a minimal Forth-style interpreter for AIOS, running in Ring 3
 *
 * "nawa" (نواة, nucleus/seed) is the seed of the system's self-extensible
 * language pillar: a language small enough to read in one sitting, in which
 * the user extends the vocabulary at runtime with `: name ... ;`.
 *
 * Built entirely on libaios — no libc, no printf, no floating point. The
 * kernel is untouched; everything below goes through the frozen syscall ABI.
 *
 * ---------------------------------------------------------------------------
 * DESIGN: how user-defined words are stored and run
 * ---------------------------------------------------------------------------
 * A definition's body is kept as its SOURCE TEXT — the tokens between the name
 * and the `;`, joined by single spaces, in one malloc'd string. Invoking a word
 * re-tokenises that string and runs each token through the very same
 * execute_token() the outer interpreter uses ("token re-parse" threading).
 *
 * Why this over storing resolved dictionary indices (the other option):
 *   - It needs no resolution pass and no separate compiled representation. The
 *     body is just text, so the tokeniser is the only parser in the program.
 *   - A word calling another word falls out for free, at any depth.
 *   - Name lookup happens at CALL time, so redefinition is "latest wins" for
 *     every caller, exactly as specified.
 *
 * The tradeoff, stated plainly: this gives LATE binding. If you redefine
 * `square`, an already-defined `cube` that calls it picks up the new one.
 * Classic Forth compiles the address at definition time and would keep the old
 * behaviour. Late binding is the simpler and more obviously-correct design at
 * this size, and it is the one the spec's "latest wins" describes; early
 * binding is noted as a v1 candidate below.
 *
 * Late binding does mean `: a a ;` becomes infinite recursion at call time,
 * which on a ONE-PAGE user stack is fatal. MAX_DEPTH caps nesting and reports
 * an error long before the stack page runs out — see interpret_text().
 *
 * ---------------------------------------------------------------------------
 * v0 SCOPE IS FROZEN. v1 CANDIDATES (deliberately absent here):
 * ---------------------------------------------------------------------------
 *   - Control flow: if/else/then, begin/until, do/loop
 *   - Variables and constants; a name→cell store
 *   - Strings as values (." ... ", counted strings)
 *   - Early binding: compile bodies to dictionary indices, plus forward
 *     references and proper recursion (`recurse`)
 *   - Return stack manipulation (>r, r>), and words that touch it
 *   - Number bases other than decimal (hex literals, `base`)
 *   - `words` to list the dictionary, `forget` to unwind it
 *   - Saving definitions back to a .nw file (needs a write syscall; the ABI is
 *     read-only today, so this one is blocked on the kernel, not on nawa)
 * ========================================================================== */

#include "aios.h"

/* --- limits (all bounded; nothing here can grow without a check) ---------- */
#define STACK_MAX     256   /* data stack cells                              */
#define DICT_MAX       64   /* user-defined words                            */
#define NAME_MAX       32   /* max word-name length, incl. NUL               */
#define LINE_MAX      512   /* REPL input line                               */
#define MAX_DEPTH      12   /* nested word invocations (4KB stack!)          */
#define DEF_INIT_CAP  128   /* initial malloc for a definition body          */

/* ==========================================================================
 * Data stack
 *
 * Every pop checks for underflow and every push checks for overflow. On
 * violation we print an error and RESET the stack: a wrong program must leave
 * the interpreter in a clean, known state rather than a half-consumed one.
 * ========================================================================== */

static int32_t g_stack[STACK_MAX];
static int32_t g_sp = 0;            /* number of cells currently held */

/* Set when an error aborts the current line/definition. Checked by the token
 * loop so the rest of a bad line is discarded rather than executed. */
static bool g_abort = false;

static void err(const char* msg) {
    print("  ?? ");
    print(msg);
    print("\n");
    g_abort = true;
}

static void stack_reset(void) {
    g_sp = 0;
}

static void push(int32_t v) {
    if (g_sp >= STACK_MAX) {
        err("stack overflow (reset)");
        stack_reset();
        return;
    }
    g_stack[g_sp++] = v;
}

/* Pops into *out. Returns false (and resets the stack) on underflow, so every
 * caller must check before using the value. */
static bool pop(int32_t* out) {
    if (g_sp <= 0) {
        err("stack underflow (reset)");
        stack_reset();
        return false;
    }
    *out = g_stack[--g_sp];
    return true;
}

/* Convenience for the many binary operators: pops b then a (Forth order). */
static bool pop2(int32_t* a, int32_t* b) {
    if (!pop(b)) return false;
    if (!pop(a)) return false;
    return true;
}

/* ==========================================================================
 * Dictionary
 *
 * Flat array of name → body-source. Redefinition REPLACES the existing entry
 * in place (freeing the old body) rather than appending a shadowing copy:
 * lookup semantics are identical — latest wins — but a program that redefines
 * a word in a loop cannot exhaust the table or leak its bodies.
 * ========================================================================== */

typedef struct {
    char  name[NAME_MAX];
    char* body;          /* malloc'd source text of the body, NUL-terminated */
} word_t;

static word_t g_dict[DICT_MAX];
static int32_t g_dict_n = 0;

static int32_t dict_find(const char* name) {
    for (int32_t i = g_dict_n - 1; i >= 0; i--)      /* newest first */
        if (strcmp(g_dict[i].name, name) == 0) return i;
    return -1;
}

/* Takes ownership of `body` (already malloc'd). Returns false if the
 * dictionary is full, in which case the caller must free the body. */
static bool dict_define(const char* name, char* body) {
    int32_t i = dict_find(name);
    if (i >= 0) {                       /* redefinition: replace in place */
        free(g_dict[i].body);
        g_dict[i].body = body;
        return true;
    }
    if (g_dict_n >= DICT_MAX) {
        err("dictionary full");
        return false;
    }
    strlcpy(g_dict[g_dict_n].name, name, NAME_MAX);
    g_dict[g_dict_n].body = body;
    g_dict_n++;
    return true;
}

/* ==========================================================================
 * Compilation state
 *
 * Global rather than per-line so a definition may span several REPL lines:
 *     nawa> : square
 *     nawa>   dup * ;
 * The body buffer grows on demand and is handed to the dictionary at `;`.
 * ========================================================================== */

static bool   g_compiling = false;
static bool   g_need_name = false;      /* next token is the new word's name */
static char   g_defname[NAME_MAX];
static char*  g_defbuf = NULL;
static size_t g_deflen = 0;
static size_t g_defcap = 0;

/* Throw away a partially-built definition. Called on `;` completion and on any
 * error, so a failed definition never leaves the interpreter in compile mode. */
static void compile_reset(void) {
    if (g_defbuf) free(g_defbuf);
    g_defbuf    = NULL;
    g_deflen    = 0;
    g_defcap    = 0;
    g_compiling = false;
    g_need_name = false;
    g_defname[0] = '\0';
}

/* Append one token plus a separating space to the body buffer. */
static bool compile_append(const char* tok) {
    size_t need = g_deflen + strlen(tok) + 2;      /* token + ' ' + NUL */
    if (need > g_defcap) {
        size_t cap = g_defcap ? g_defcap * 2 : DEF_INIT_CAP;
        while (cap < need) cap *= 2;
        char* nb = (char*)realloc(g_defbuf, cap);
        if (!nb) { err("out of memory in definition"); return false; }
        g_defbuf = nb;
        g_defcap = cap;
    }
    size_t n = strlen(tok);
    memcpy(g_defbuf + g_deflen, tok, n);
    g_deflen += n;
    g_defbuf[g_deflen++] = ' ';
    g_defbuf[g_deflen]   = '\0';
    return true;
}

/* ==========================================================================
 * Tokeniser
 *
 * Copies the next whitespace-delimited token into `out` and returns the
 * position just past it, or NULL when the input is exhausted. A `\` token
 * starts a comment that runs to end of line.
 *
 * Tokens longer than NAME_MAX-1 are truncated; the remainder is skipped, so a
 * pathological identifier cannot overflow anything or desynchronise the parse.
 * ========================================================================== */

static const char* next_token(const char* s, char* out, size_t outsz) {
    for (;;) {
        while (*s && is_space(*s)) s++;
        if (!*s) return NULL;

        /* Comment: `\` followed by whitespace (or end) runs to end of line. */
        if (*s == '\\' && (s[1] == '\0' || is_space(s[1]))) {
            while (*s && *s != '\n') s++;
            continue;                       /* look for a token on the next line */
        }

        size_t n = 0;
        while (*s && !is_space(*s)) {
            if (n + 1 < outsz) out[n++] = *s;
            s++;                            /* keep consuming even when full */
        }
        out[n] = '\0';
        return s;
    }
}

/* ==========================================================================
 * Primitives
 * ========================================================================== */

static void prim_dot(void) {                 /* .  — pop and print */
    int32_t v;
    if (!pop(&v)) return;
    print_dec(v);
    print(" ");
}

static void prim_dots(void) {                /* .s — print the whole stack */
    print("<");
    print_udec((uint32_t)g_sp);
    print("> ");
    for (int32_t i = 0; i < g_sp; i++) {
        print_dec(g_stack[i]);
        print(" ");
    }
}

static void prim_emit(void) {                /* emit — pop, print as char */
    int32_t v;
    if (!pop(&v)) return;
    char c = (char)(v & 0x7F);
    /* SYS_WRITE stops at a NUL byte, so emitting 0 would print nothing at all
     * rather than a stray character. Show it as a space instead. */
    if (c == '\0') c = ' ';
    print_n(&c, 1);
}

static void prim_dup(void) {
    if (g_sp <= 0) { err("stack underflow (reset)"); stack_reset(); return; }
    push(g_stack[g_sp - 1]);
}

static void prim_drop(void) {
    int32_t v;
    (void)pop(&v);
}

static void prim_swap(void) {
    int32_t a, b;
    if (!pop2(&a, &b)) return;
    push(b);
    push(a);
}

static void prim_over(void) {
    if (g_sp < 2) { err("stack underflow (reset)"); stack_reset(); return; }
    push(g_stack[g_sp - 2]);
}

static void prim_add(void) { int32_t a,b; if (pop2(&a,&b)) push(a + b); }
static void prim_sub(void) { int32_t a,b; if (pop2(&a,&b)) push(a - b); }
static void prim_mul(void) { int32_t a,b; if (pop2(&a,&b)) push(a * b); }

/* Division and modulo guard against BOTH divide-by-zero and the one signed
 * overflow case on i386: INT32_MIN / -1 traps at the hardware level (#DE),
 * which would kill the process. Neither pushes a result. */
static void prim_div(void) {
    int32_t a, b;
    if (!pop2(&a, &b)) return;
    if (b == 0) { err("division by zero"); return; }
    if (a == (int32_t)0x80000000 && b == -1) { err("division overflow"); return; }
    push(a / b);
}

static void prim_mod(void) {
    int32_t a, b;
    if (!pop2(&a, &b)) return;
    if (b == 0) { err("division by zero"); return; }
    if (a == (int32_t)0x80000000 && b == -1) { err("division overflow"); return; }
    push(a % b);
}

/* Comparison words push Forth flags: 0 = false, -1 (all bits set) = true. */
static void prim_eq(void) { int32_t a,b; if (pop2(&a,&b)) push(a == b ? -1 : 0); }
static void prim_lt(void) { int32_t a,b; if (pop2(&a,&b)) push(a <  b ? -1 : 0); }
static void prim_gt(void) { int32_t a,b; if (pop2(&a,&b)) push(a >  b ? -1 : 0); }

static void prim_cr(void)  { print("\n"); }
static void prim_bye(void) { print("\n  nawa: bye.\n"); sys_exit(); }

typedef void (*prim_fn)(void);

typedef struct {
    const char* name;
    prim_fn     fn;
} prim_t;

/* Order is irrelevant to correctness (names are unique); it is grouped by kind
 * for readability. Primitives cannot be redefined by the user — dict_find is
 * consulted first, so a user definition SHADOWS a primitive if they insist. */
static const prim_t g_prims[] = {
    { "+",    prim_add   }, { "-",    prim_sub  },
    { "*",    prim_mul   }, { "/",    prim_div  },
    { "mod",  prim_mod   },
    { ".",    prim_dot   }, { ".s",   prim_dots },
    { "dup",  prim_dup   }, { "drop", prim_drop },
    { "swap", prim_swap  }, { "over", prim_over },
    { "=",    prim_eq    }, { "<",    prim_lt   }, { ">", prim_gt },
    { "emit", prim_emit  }, { "cr",   prim_cr   },
    { "bye",  prim_bye   },
};

#define PRIM_COUNT ((int32_t)(sizeof(g_prims) / sizeof(g_prims[0])))

static const prim_t* prim_find(const char* name) {
    for (int32_t i = 0; i < PRIM_COUNT; i++)
        if (strcmp(g_prims[i].name, name) == 0) return &g_prims[i];
    return NULL;
}

/* ==========================================================================
 * Interpreter core
 * ========================================================================== */

static void interpret_text(const char* src, int32_t depth);

/* Execute one already-tokenised word. `depth` is the current nesting level and
 * is only used to bound the C recursion that user-word invocation causes. */
static void execute_token(const char* tok, int32_t depth) {
    /* 1. A number? Push it. Checked first so a program cannot shadow a
     *    literal by defining a word called "42". */
    int32_t value;
    if (str_to_i32(tok, &value)) { push(value); return; }

    /* 2. A user-defined word? Re-interpret its body. Consulted before the
     *    primitives so a user definition shadows a builtin of the same name. */
    int32_t di = dict_find(tok);
    if (di >= 0) {
        interpret_text(g_dict[di].body, depth + 1);
        return;
    }

    /* 3. A primitive? */
    const prim_t* p = prim_find(tok);
    if (p) { p->fn(); return; }

    /* 4. Unknown. Report it and abort the rest of the line. */
    print("  ? ");
    print(tok);
    print("\n");
    g_abort = true;
}

/* Tokenise `src` and run it. Handles `:` / `;` compilation state, which is
 * global so definitions may span input lines.
 *
 * Every frame here costs one NAME_MAX token buffer plus a few pointers, and
 * MAX_DEPTH bounds how many can be live at once — the whole chain stays well
 * inside the single 4KB user stack page. */
static void interpret_text(const char* src, int32_t depth) {
    if (depth > MAX_DEPTH) {
        err("nesting too deep (runaway recursion?)");
        return;
    }

    char tok[NAME_MAX];
    const char* s = src;

    while ((s = next_token(s, tok, sizeof(tok))) != NULL) {
        if (g_abort) return;                 /* discard the rest of the line */

        /* --- compiling a definition body --------------------------------- */
        if (g_compiling) {
            if (g_need_name) {
                /* The token straight after `:` names the word. Refuse names
                 * that would be unreachable or confusing. */
                if (strcmp(tok, ";") == 0) {
                    err("`:` needs a name");
                    compile_reset();
                    continue;
                }
                int32_t dummy;
                if (str_to_i32(tok, &dummy)) {
                    err("a word name cannot be a number");
                    compile_reset();
                    continue;
                }
                strlcpy(g_defname, tok, NAME_MAX);
                g_need_name = false;
                continue;
            }

            if (strcmp(tok, ";") == 0) {     /* end of definition */
                char* body = g_defbuf ? g_defbuf : (char*)malloc(1);
                if (!body) { err("out of memory"); compile_reset(); continue; }
                if (!g_defbuf) body[0] = '\0';   /* an empty body is legal */

                if (dict_define(g_defname, body)) {
                    print("  ok ");
                    print(g_defname);
                    print("\n");
                    g_defbuf = NULL;             /* ownership transferred */
                } else {
                    free(body);
                    if (g_defbuf == body) g_defbuf = NULL;
                }
                compile_reset();
                continue;
            }

            if (strcmp(tok, ":") == 0) {     /* v0 has no nested definitions */
                err("nested `:` is not supported");
                compile_reset();
                continue;
            }

            if (!compile_append(tok)) compile_reset();
            continue;
        }

        /* --- interpreting ------------------------------------------------- */
        if (strcmp(tok, ":") == 0) {
            g_compiling  = true;
            g_need_name  = true;
            g_defname[0] = '\0';
            continue;
        }

        if (strcmp(tok, ";") == 0) {
            err("`;` outside a definition");
            continue;
        }

        execute_token(tok, depth);
    }
}

/* One unit of input: run it, then clear the abort flag so the NEXT line still
 * runs. An error during compilation also abandons the definition — otherwise
 * the user would be silently stuck in compile mode. */
static void interpret_line(const char* line) {
    g_abort = false;
    interpret_text(line, 0);
    if (g_abort && g_compiling) {
        print("  (definition abandoned)\n");
        compile_reset();
    }
    g_abort = false;
}

/* ==========================================================================
 * Input
 * ========================================================================== */

/* Whether the idle poll loop calls SYS_YIELD.
 *
 * This SHOULD be 1. It is 0 because SYS_YIELD from Ring 3 currently panics the
 * kernel — see the long comment on poll_idle() below. Flip it back to 1 the
 * day the scheduler is fixed; nothing else in nawa needs to change. */
#define NAWA_POLL_YIELD 0

/* Called when the keyboard queue is empty.
 *
 * ------------------------------------------------------------------------
 * WHY THIS DOES NOT CALL SYS_YIELD, EVEN THOUGH IT OBVIOUSLY SHOULD
 * ------------------------------------------------------------------------
 * SYS_READ is non-blocking (ABI.md): it returns 0 when no character is queued.
 * The correct way to wait is therefore SYS_YIELD, so the rest of the system
 * gets the CPU instead of us spinning through our whole timeslice.
 *
 * That path is currently BROKEN IN THE KERNEL. A Ring 3 process that calls
 * SYS_YIELD in a loop reliably triggers:
 *
 *     AIOS KERNEL PANIC - Exception: Invalid Opcode (#6)
 *     EIP=0x00000003  CS=0x00000008
 *
 * i.e. the kernel returns to a garbage address. Measured, with everything else
 * held constant:
 *
 *     200000 x SYS_YIELD ................. PANIC (also panics with -smp 1,
 *                                          so it is not an SMP race)
 *     40 x SYS_YIELD ..................... fine
 *     200000 x SYS_UPTIME (no yield) ..... fine
 *     563628 x SYS_READ over 12s (no
 *       yield, timer preempting us the
 *       whole time) ...................... fine
 *
 * So it is not syscall volume, not process lifetime, and not preemption — it
 * is specifically schedule()/switch_context() reached from the int 0x80
 * handler. Vector 128 is an INTERRUPT gate (idt.c sets flags 0xEE), so the
 * syscall runs with IF clear, but schedule() executes an unconditional `sti`
 * and clears its `in_schedule` re-entry guard immediately before calling
 * switch_context(). A timer IRQ landing in that window re-enters the scheduler
 * mid-stack-switch and corrupts a saved ESP. The timer path calls schedule()
 * the same way but only at 100 Hz, which is why it survives while a poll loop
 * at ~50k yields/sec does not.
 *
 * Fixing that means changing the kernel scheduler, which this task explicitly
 * forbids, so nawa busy-polls instead. The cost is real — we burn our
 * timeslice — but the timer still preempts us, so the shell and every other
 * process keep running normally (verified above). A hung machine would be far
 * worse than a warm one.
 * ------------------------------------------------------------------------ */
static void poll_idle(void) {
#if NAWA_POLL_YIELD
    sys_yield();
#endif
}

/* Read one line into `buf`, echoing as we go.
 *
 * The kernel does not echo for us here, so every accepted character is written
 * back explicitly.
 *
 * Over-long lines are truncated but still consumed to the newline, so the tail
 * of a runaway line cannot leak into the next one. */
static void read_line(char* buf, size_t max) {
    size_t n = 0;
    bool truncated = false;

    for (;;) {
        char c;
        int32_t r = sys_read(&c, 1);
        if (r <= 0) {
            poll_idle();                 /* nothing queued: wait politely */
            continue;
        }

        if (c == '\n') {
            print("\n");
            buf[n] = '\0';
            if (truncated) print("  ?? line too long (truncated)\n");
            return;
        }

        if (c == '\b') {                 /* backspace */
            if (n > 0) { n--; print("\b"); }
            continue;
        }

        if (c < 32 || c > 126) continue; /* ignore control / extended keys */

        if (n + 1 < max) {
            buf[n++] = c;
            print_n(&c, 1);              /* the kernel does not echo for us */
        } else {
            truncated = true;            /* keep consuming to the newline */
        }
    }
}

/* ==========================================================================
 * File mode
 * ========================================================================== */

/* Slurp a VFS file into a malloc'd, NUL-terminated buffer. Returns NULL if the
 * file does not exist, is empty, or memory runs out. */
static char* read_whole_file(const char* path, uint32_t* out_len) {
    int32_t fd = sys_open(path);
    if (fd < 0) return NULL;

    uint32_t cap = 2048;
    char* buf = (char*)malloc(cap);
    if (!buf) { sys_fclose(fd); return NULL; }

    uint32_t len = 0;
    for (;;) {
        if (len + 512 > cap) {              /* keep headroom for one read */
            uint32_t ncap = cap * 2;
            char* nb = (char*)realloc(buf, ncap);
            if (!nb) { free(buf); sys_fclose(fd); return NULL; }
            buf = nb;
            cap = ncap;
        }
        int32_t n = sys_fread(fd, buf + len, 512);
        if (n <= 0) break;                  /* 0 == EOF, negative == error */
        len += (uint32_t)n;
    }
    sys_fclose(fd);

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Interpret a script line by line. Splitting on newlines (rather than handing
 * the whole text to interpret_text at once) keeps error recovery per-line:
 * a bad line is discarded and the script carries on. */
static void run_script(const char* path) {
    uint32_t len = 0;
    char* text = read_whole_file(path, &len);
    if (!text) return;                      /* no boot file: not an error */

    print("  nawa: loading ");
    print(path);
    print(" (");
    print_udec(len);
    print(" bytes)\n");

    char* line = text;
    for (uint32_t i = 0; i <= len; i++) {
        if (text[i] == '\n' || text[i] == '\0') {
            text[i] = '\0';
            if (line[0]) interpret_line(line);
            line = text + i + 1;
        }
    }

    /* A definition left open at EOF would silently swallow the first REPL
     * line, so close it out loudly instead. */
    if (g_compiling) {
        print("  ?? unterminated `:` at end of file (abandoned)\n");
        compile_reset();
    }

    free(text);
    print("  nawa: boot script done.\n");
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void) {
    print("\n");
    print("  ==============================================\n");
    print("  |  nawa v0 - a small Forth for AIOS          |\n");
    print("  |  : name ... ;  defines.   bye  exits.      |\n");
    print("  ==============================================\n");

    /* File mode first: if boot.nw exists, run it, then fall through to the
     * REPL (EOF is not an exit). */
    run_script("boot.nw");

    /* The input line lives on the HEAP, not the stack: LINE_MAX is 512 bytes
     * and the whole user stack is one 4KB page. */
    char* line = (char*)malloc(LINE_MAX);
    if (!line) {
        print("  nawa: out of memory for input buffer\n");
        return 1;
    }

    print("\n  Type words separated by spaces. `bye` to exit.\n");

    for (;;) {
        print(g_compiling ? "  ...> " : "  nawa> ");
        read_line(line, LINE_MAX);
        interpret_line(line);
        if (!g_compiling) print("  ok\n");
    }

    /* Unreachable: `bye` exits via SYS_EXIT. */
}
