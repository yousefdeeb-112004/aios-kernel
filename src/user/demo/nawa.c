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
 * CONTROL FLOW (v1) is now implemented — if/else/then, begin/until, do/loop —
 * see the "Control flow" section further down for how it is resolved against
 * the body TEXT rather than a compiled instruction stream.
 *
 * MEMORY (v1) is now implemented — variable / constant / @ / ! / +! / cells —
 * see the "Memory" section for the safe-by-construction address model.
 *
 * v2 CANDIDATES STILL ABSENT:
 * ---------------------------------------------------------------------------
 *   - Arrays and `allot`; strings as values (." ... ", counted strings)
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
#define CTRL_MAX       32   /* nested begin/do loop frames at run time        */

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
    bool  immediate;     /* runs at COMPILE time instead of being compiled     */
} word_t;

static word_t g_dict[DICT_MAX];
static int32_t g_dict_n       = 0;
static int32_t g_last_def_idx = -1;     /* index of most recently (re)defined word */

static int32_t dict_find(const char* name) {
    for (int32_t i = g_dict_n - 1; i >= 0; i--)      /* newest first */
        if (strcmp(g_dict[i].name, name) == 0) return i;
    return -1;
}

/* Takes ownership of `body` (already malloc'd). Returns false if the
 * dictionary is full, in which case the caller must free the body. New entries
 * are non-immediate; the colon compiler sets the flag afterwards if asked. */
static bool dict_define(const char* name, char* body) {
    int32_t i = dict_find(name);
    if (i >= 0) {                       /* redefinition: replace in place */
        free(g_dict[i].body);
        g_dict[i].body      = body;
        g_dict[i].immediate = false;
        g_last_def_idx      = i;
        return true;
    }
    if (g_dict_n >= DICT_MAX) {
        err("dictionary full");
        return false;
    }
    strlcpy(g_dict[g_dict_n].name, name, NAME_MAX);
    g_dict[g_dict_n].body      = body;
    g_dict[g_dict_n].immediate = false;
    g_last_def_idx             = g_dict_n;
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
static bool   g_def_immediate = false;  /* word being compiled asked for `immediate` */
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
    g_def_immediate = false;
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
 * Memory: variables, constants, and fetch/store (@ ! +!)
 *
 * SAFE BY CONSTRUCTION — the "governed" property of the language pillar.
 * `variable` cells live in ONE malloc'd arena owned by the interpreter. `@`,
 * `!` and `+!` receive an arbitrary integer as an address, so before every
 * access we bound-check it against that arena's live range (addr_ok): the
 * address must land inside [arena, arena + used*4) AND on a 4-byte cell
 * boundary. Any other value — a wild integer, a stale or foreign pointer, a
 * misaligned address — is rejected with `bad-addr` and the line recovers. A
 * nawa program therefore CANNOT read or write a single byte outside its own
 * variable space, no matter what it puts on the stack. This governance sits
 * ABOVE the kernel's Ring 3 isolation: even within its own legal address
 * space, nawa fences itself in.
 *
 * `constant` needs no arena — it defines a word whose body is the literal
 * value, so invoking it just pushes that number. `variable` is the same trick
 * with the cell's ADDRESS as the literal: both are ordinary numeric-literal
 * words, which is exactly why they compose with the rest of the language for
 * free (a variable's address flows through the stack like any other number).
 * ========================================================================== */

#define VARS_MAX 64             /* cells in the variable arena */

static int32_t* g_arena   = NULL;   /* malloc'd on first `variable`, never freed */
static int32_t  g_arena_n = 0;      /* cells handed out so far                   */

/* The single gate between a nawa integer and a real memory access. Returns true
 * iff `addr` names a live cell; on success *cell points at it. */
static bool addr_ok(int32_t addr, int32_t** cell) {
    if (!g_arena) return false;
    uint32_t base = (uint32_t)g_arena;
    uint32_t a    = (uint32_t)addr;
    uint32_t end  = base + (uint32_t)g_arena_n * 4u;
    if (a < base || a >= end)   return false;   /* outside the arena       */
    if (((a - base) & 3u) != 0) return false;   /* not on a cell boundary  */
    if (cell) *cell = (int32_t*)a;
    return true;
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

/* Memory access. Every address passes through addr_ok first, so a bad address
 * is a caught `bad-addr` error — never a fault, never a stray write. */
static void prim_fetch(void) {               /* @  ( addr -- x ) */
    int32_t addr;
    if (!pop(&addr)) return;
    int32_t* cell;
    if (!addr_ok(addr, &cell)) { err("bad-addr"); return; }
    push(*cell);
}
static void prim_store(void) {               /* !  ( x addr -- ) */
    int32_t addr, val;
    if (!pop(&addr)) return;                 /* addr is on top */
    if (!pop(&val)) return;
    int32_t* cell;
    if (!addr_ok(addr, &cell)) { err("bad-addr"); return; }
    *cell = val;
}
static void prim_addstore(void) {            /* +! ( n addr -- ) */
    int32_t addr, n;
    if (!pop(&addr)) return;
    if (!pop(&n)) return;
    int32_t* cell;
    if (!addr_ok(addr, &cell)) { err("bad-addr"); return; }
    *cell += n;
}
static void prim_cells(void) {               /* cells ( n -- n*4 ) */
    int32_t n;
    if (pop(&n)) push(n * 4);
}

/* ---- self-extension: compiler-access words (see "governed self-extension") - */

static void prim_zeroeq(void) {              /* 0= ( x -- flag )  true iff x==0 */
    int32_t v;
    if (pop(&v)) push(v == 0 ? -1 : 0);
}

/* , ( x -- ) — compile the popped value into the current definition as a
 * literal. Only meaningful while compiling (an immediate word's body runs at
 * compile time); used outside a definition it is a caught error. */
static void prim_comma(void) {
    int32_t v;
    if (!pop(&v)) return;                    /* underflow: err+reset already done */
    if (!g_compiling) { err("`,` only while compiling"); return; }
    char num[16];
    itoa(v, num, sizeof(num), 10);
    if (!compile_append(num)) err("out of memory compiling literal");
}

/* immediate — mark the newest word immediate. Inside a definition it flags the
 * word being compiled (applied at `;`); at the top level it flags the most
 * recently defined word. With nothing to mark it is a caught error, never a
 * crash. */
static void prim_immediate(void) {
    if (g_compiling) {
        g_def_immediate = true;
    } else if (g_last_def_idx >= 0 && g_last_def_idx < g_dict_n) {
        g_dict[g_last_def_idx].immediate = true;
    } else {
        err("immediate: no word to mark");
    }
}

/* --- cross-layer: read the kernel AI agent's live Bayesian state ------------
 * The first thread from the language pillar to the AI pillar. `ai-p1` / `ai-p2`
 * push node 1 / node 2's current posterior permille; `ai-anom` pushes the
 * anomaly bitmask. Each calls ai_stat() FRESH every time (no caching), so a
 * nawa program always reasons about the LIVE agent. READ-ONLY by construction:
 * SYS_AISTAT only snapshots, so nawa observes the agent and cannot steer it.
 * On syscall failure they push 0 and print a warning ONCE. */
static bool g_ai_warned = false;

static bool ai_snapshot(ai_stat_t* st) {
    if (ai_stat(st) == 0) return true;
    if (!g_ai_warned) { print("  ?? ai-stat syscall failed (pushing 0)\n"); g_ai_warned = true; }
    push(0);
    return false;
}

static void prim_ai_p1(void)   { ai_stat_t s; if (ai_snapshot(&s)) push((int32_t)s.n1_permille); }
static void prim_ai_p2(void)   { ai_stat_t s; if (ai_snapshot(&s)) push((int32_t)s.n2_permille); }
static void prim_ai_anom(void) { ai_stat_t s; if (ai_snapshot(&s)) push((int32_t)s.anomalies); }

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
    { "0=",   prim_zeroeq },
    { "@",    prim_fetch }, { "!",    prim_store },
    { "+!",   prim_addstore }, { "cells", prim_cells },
    { ",",    prim_comma }, { "immediate", prim_immediate },
    { "ai-p1", prim_ai_p1 }, { "ai-p2", prim_ai_p2 }, { "ai-anom", prim_ai_anom },
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
 * Control flow (v1): if/else/then, begin/until, do/loop
 *
 * nawa executes a word by RE-TOKENISING its body text (see DESIGN up top), so
 * there is no compiled instruction array to patch branch offsets into. Control
 * flow is resolved against the body TEXT at execution time:
 *   - if/else/then — forward-scan (skip_if_false / skip_to_then); no runtime
 *     state, nesting handled by a depth counter over if/then.
 *   - begin/until, do/loop — a small run-time control stack remembers the body
 *     cursor to resume at. Looping just RESETS that cursor inside the single
 *     execute_body() loop: iteration in the VM, never C recursion, so a
 *     million-iteration loop still uses O(1) C stack.
 * Balance is VALIDATED once at `;` (validate_body): an unclosed if/begin/do or
 * a stray closer aborts that definition cleanly instead of compiling a word
 * that would misbehave at call time. The words are compile-only; used at the
 * top level they report an error and the line recovers.
 * ========================================================================== */

enum { CTRL_BEGIN, CTRL_DO };

typedef struct {
    int32_t     type;      /* CTRL_BEGIN or CTRL_DO                          */
    const char* loop_pos;  /* body cursor to resume at (just past begin/do)  */
    int32_t     index;     /* do: current counter (what `i` pushes)          */
    int32_t     limit;     /* do: exclusive upper bound                      */
} ctrl_t;

static ctrl_t  g_ctrl[CTRL_MAX];
static int32_t g_ctrl_n = 0;

static bool is_ctrl_word(const char* t) {
    return strcmp(t, "if")    == 0 || strcmp(t, "else")  == 0 ||
           strcmp(t, "then")  == 0 || strcmp(t, "begin") == 0 ||
           strcmp(t, "until") == 0 || strcmp(t, "do")    == 0 ||
           strcmp(t, "loop")  == 0 || strcmp(t, "i")     == 0;
}

/* Shared name check for the defining words `variable` and `constant`: a name
 * must exist, must not be a bare number (numbers are read as literals, so the
 * word would be unreachable) and must not be a control word. */
static bool defname_ok(const char* name) {
    if (name[0] == '\0')      { err("missing name");                   return false; }
    int32_t d;
    if (str_to_i32(name, &d)) { err("a name cannot be a number");      return false; }
    if (is_ctrl_word(name))   { err("a name cannot be a control word"); return false; }
    return true;
}

/* Define `name` as a word whose body is the decimal literal `value`. This is
 * the whole implementation of both `constant` (value = the number) and
 * `variable` (value = a cell address): invoking such a word pushes `value`. */
static void define_literal_word(const char* name, int32_t value) {
    char num[16];
    itoa(value, num, sizeof(num), 10);
    size_t n = strlen(num);
    char* body = (char*)malloc(n + 1);
    if (!body) { err("out of memory"); return; }
    memcpy(body, num, n + 1);
    if (dict_define(name, body)) {
        print("  ok ");
        print(name);
        print("\n");
    } else {
        free(body);          /* dictionary full: dict_define already reported it */
    }
}

/* True if `name` denotes something nawa can run: a number literal, a control
 * word, a user word, or a primitive. `postpone` uses it to reject compiling a
 * reference to a word that does not exist. */
static bool word_known(const char* name) {
    int32_t d;
    if (str_to_i32(name, &d))  return true;   /* a literal */
    if (is_ctrl_word(name))    return true;   /* runtime control word */
    if (dict_find(name) >= 0)  return true;
    if (prim_find(name) != NULL) return true;
    return false;
}

/* ==========================================================================
 * Governed self-extension (v1) — the language extending its own compiler
 *
 * An IMMEDIATE word runs at COMPILE time (while a `: ... ;` body is being
 * built) instead of being compiled into it. That is the whole trick by which a
 * nawa program adds new *syntax*: an immediate word executes during
 * compilation and emits tokens into the definition under construction, via
 *   postpone <word>   append a reference to <word>   (the "[compile]" word)
 *   ,                 append the popped value as a literal
 * So `: unless immediate postpone 0= postpone if ;` makes `unless` an immediate
 * word whose body is `postpone 0= postpone if`; when a LATER definition uses
 * `unless`, it runs and compiles `0= if` in place — a brand-new control word,
 * written in nawa, in nawa's own source.
 *
 * INVARIANTS that hold no matter what a nawa program does at compile time —
 * because compile-time code runs through the exact same executor as run-time
 * code, inside the same safety envelope:
 *   - The data stack is bounds-checked on every push/pop; underflow/overflow
 *     is a caught error that resets the stack, never a wild access.
 *   - @ / ! / +! are still gated by addr_ok — an immediate word cannot reach
 *     outside the variable arena either.
 *   - Any error at compile time sets g_abort; interpret_line then ABANDONS the
 *     half-built definition cleanly (compile_reset) — the interpreter never
 *     gets stuck in compile mode and never compiles a corrupt body.
 *   - `,` and `postpone` refuse to run outside a definition; `postpone` refuses
 *     unknown words. Malformed input is rejected, not executed.
 *   - No metacompilation, no DOES>, no code generation: "compiling" only ever
 *     APPENDS validated token text, so a compile-time word can do nothing a
 *     run-time word could not already do safely.
 * The result is self-extension that is *governed*: unbounded expressiveness in
 * what syntax a program may define, zero new ways to escape the safety model.
 *
 * NOTE on control flow (task item 2): nawa's if/else/then/begin/until/do/loop
 * are RUN-TIME words, resolved by execute_body scanning the body text (phase
 * 9's text-reparse model) — they are deliberately NOT compile-time immediates
 * the way classic Forth's are, because nawa compiles to TEXT, not to a branch-
 * patchable instruction stream. They are therefore not parser special-cases in
 * the COMPILE path (that path already treats them as ordinary compiled tokens);
 * there is nothing to refactor there. The immediate mechanism's reality is
 * proven instead by `unless` above — genuinely new syntax built from it.
 *
 * v3 CANDIDATES (deliberately absent): `[` / `]` interpret-inside-compile
 * brackets (deferred — not needed by any word here); DOES> / create; string
 * literals; target-compilation / metacompilation.
 * ========================================================================== */

/* ==========================================================================
 * Interpreter core
 * ========================================================================== */

static void interpret_text(const char* src, int32_t depth);
static void execute_body(const char* body, int32_t depth);

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
        execute_body(g_dict[di].body, depth + 1);
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

/* From a body cursor just past a FALSE `if`, return the cursor just past the
 * matching `else` (start of the else-branch) or `then` (no else). Nested
 * if/then are skipped via `nest`. Returns NULL if the `if` is unterminated —
 * validate_body() rules that out at `;`, but the runtime check costs nothing. */
static const char* skip_if_false(const char* s) {
    int32_t nest = 0;
    char t[NAME_MAX];
    const char* next;
    while ((next = next_token(s, t, sizeof(t))) != NULL) {
        if (strcmp(t, "if") == 0) {
            nest++;
        } else if (strcmp(t, "then") == 0) {
            if (nest == 0) return next;
            nest--;
        } else if (strcmp(t, "else") == 0 && nest == 0) {
            return next;
        }
        s = next;
    }
    return NULL;
}

/* From a body cursor just past an `else` reached on the TRUE path, return the
 * cursor just past the matching `then`. */
static const char* skip_to_then(const char* s) {
    int32_t nest = 0;
    char t[NAME_MAX];
    const char* next;
    while ((next = next_token(s, t, sizeof(t))) != NULL) {
        if (strcmp(t, "if") == 0) {
            nest++;
        } else if (strcmp(t, "then") == 0) {
            if (nest == 0) return next;
            nest--;
        }
        s = next;
    }
    return NULL;
}

/* Execute a user word's body: the control-flow-aware executor. Same token
 * re-parse as interpret_text, but it recognises the control words and loops by
 * RESETTING the text cursor `s` (no C recursion per iteration). Each frame
 * records the control-stack depth it inherited and truncates back to it on the
 * way out — including on g_abort — so a loop left half-open by an error can
 * never corrupt an enclosing word's loop frames. */
static void execute_body(const char* body, int32_t depth) {
    if (depth > MAX_DEPTH) {
        err("nesting too deep (runaway recursion?)");
        return;
    }

    int32_t ctrl_base = g_ctrl_n;      /* this frame owns nothing below here */
    char tok[NAME_MAX];
    const char* s = body;

    while ((s = next_token(s, tok, sizeof(tok))) != NULL) {
        if (g_abort) break;

        if (strcmp(tok, "if") == 0) {
            int32_t f;
            if (!pop(&f)) break;
            if (f == 0) {                          /* false: skip the branch */
                const char* n = skip_if_false(s);
                if (!n) { err("`if` without `then`"); break; }
                s = n;
            }
            continue;                              /* true: fall through */
        }
        if (strcmp(tok, "else") == 0) {            /* hit only on the true path */
            const char* n = skip_to_then(s);
            if (!n) { err("`else` without `then`"); break; }
            s = n;
            continue;
        }
        if (strcmp(tok, "then") == 0) {            /* structural no-op */
            continue;
        }
        if (strcmp(tok, "begin") == 0) {
            if (g_ctrl_n >= CTRL_MAX) { err("loops nested too deep"); break; }
            g_ctrl[g_ctrl_n].type     = CTRL_BEGIN;
            g_ctrl[g_ctrl_n].loop_pos = s;         /* resume just past begin */
            g_ctrl_n++;
            continue;
        }
        if (strcmp(tok, "until") == 0) {
            if (g_ctrl_n <= ctrl_base || g_ctrl[g_ctrl_n - 1].type != CTRL_BEGIN) {
                err("`until` without `begin`"); break;
            }
            int32_t f;
            if (!pop(&f)) break;
            if (f == 0) {                          /* flag false: loop back */
                s = g_ctrl[g_ctrl_n - 1].loop_pos;
            } else {                               /* flag true: leave loop */
                g_ctrl_n--;
            }
            continue;
        }
        if (strcmp(tok, "do") == 0) {              /* ( limit start -- ) */
            int32_t start, limit;
            if (!pop(&start)) break;
            if (!pop(&limit)) break;
            if (g_ctrl_n >= CTRL_MAX) { err("loops nested too deep"); break; }
            g_ctrl[g_ctrl_n].type     = CTRL_DO;
            g_ctrl[g_ctrl_n].loop_pos = s;
            g_ctrl[g_ctrl_n].index    = start;
            g_ctrl[g_ctrl_n].limit    = limit;
            g_ctrl_n++;
            continue;
        }
        if (strcmp(tok, "loop") == 0) {
            if (g_ctrl_n <= ctrl_base || g_ctrl[g_ctrl_n - 1].type != CTRL_DO) {
                err("`loop` without `do`"); break;
            }
            g_ctrl[g_ctrl_n - 1].index++;
            if (g_ctrl[g_ctrl_n - 1].index < g_ctrl[g_ctrl_n - 1].limit) {
                s = g_ctrl[g_ctrl_n - 1].loop_pos; /* another iteration */
            } else {
                g_ctrl_n--;                        /* counted out */
            }
            continue;
        }
        if (strcmp(tok, "i") == 0) {               /* innermost do index */
            int32_t k = g_ctrl_n - 1;
            while (k >= ctrl_base && g_ctrl[k].type != CTRL_DO) k--;
            if (k < ctrl_base) { err("`i` outside `do` .. `loop`"); break; }
            push(g_ctrl[k].index);
            continue;
        }
        if (strcmp(tok, "postpone") == 0) {        /* compile a ref to next word */
            char nm[NAME_MAX];
            s = next_token(s, nm, sizeof(nm));
            if (!s)             { err("postpone needs a word");        break; }
            if (!g_compiling)   { err("postpone: only while compiling"); break; }
            if (!word_known(nm)){ err("postpone: unknown word");       break; }
            if (!compile_append(nm)) { err("out of memory compiling"); break; }
            continue;
        }

        execute_token(tok, depth);
    }

    g_ctrl_n = ctrl_base;    /* drop this frame's loop frames (also on abort) */
}

/* Check control-structure balance in a finished body, once, at `;`. Returns
 * false (after printing an error) on any unclosed if/begin/do, a stray closer,
 * or `i` used with no enclosing do..loop. The compile-time stack is a plain
 * local array — this runs once and never recurses. */
static bool validate_body(const char* body) {
    char cstack[CTRL_MAX];   /* 'I' if, 'E' else, 'B' begin, 'D' do */
    int32_t csn = 0;
    char t[NAME_MAX];
    const char* s = body;

    while ((s = next_token(s, t, sizeof(t))) != NULL) {
        if (strcmp(t, "postpone") == 0) {
            /* The token after `postpone` is its ARGUMENT (a word name being
             * compiled as data), not a control word — skip it so a body like
             * `postpone if` does not read as an unbalanced `if`. */
            const char* ns = next_token(s, t, sizeof(t));
            if (!ns) break;
            s = ns;
            continue;
        }
        if (strcmp(t, "if") == 0) {
            if (csn >= CTRL_MAX) { err("control nesting too deep"); return false; }
            cstack[csn++] = 'I';
        } else if (strcmp(t, "else") == 0) {
            if (csn == 0 || cstack[csn - 1] != 'I') { err("`else` without `if`"); return false; }
            cstack[csn - 1] = 'E';
        } else if (strcmp(t, "then") == 0) {
            if (csn == 0 || (cstack[csn - 1] != 'I' && cstack[csn - 1] != 'E')) {
                err("`then` without `if`"); return false;
            }
            csn--;
        } else if (strcmp(t, "begin") == 0) {
            if (csn >= CTRL_MAX) { err("control nesting too deep"); return false; }
            cstack[csn++] = 'B';
        } else if (strcmp(t, "until") == 0) {
            if (csn == 0 || cstack[csn - 1] != 'B') { err("`until` without `begin`"); return false; }
            csn--;
        } else if (strcmp(t, "do") == 0) {
            if (csn >= CTRL_MAX) { err("control nesting too deep"); return false; }
            cstack[csn++] = 'D';
        } else if (strcmp(t, "loop") == 0) {
            if (csn == 0 || cstack[csn - 1] != 'D') { err("`loop` without `do`"); return false; }
            csn--;
        } else if (strcmp(t, "i") == 0) {
            bool in_do = false;
            for (int32_t k = csn - 1; k >= 0; k--) if (cstack[k] == 'D') { in_do = true; break; }
            if (!in_do) { err("`i` outside `do` .. `loop`"); return false; }
        }
    }
    if (csn != 0) { err("unclosed if/begin/do in definition"); return false; }
    return true;
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
                if (is_ctrl_word(tok)) {
                    err("a word name cannot be a control word");
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

                if (!validate_body(body)) {      /* unbalanced if/begin/do */
                    free(body);
                    if (g_defbuf == body) g_defbuf = NULL;
                    compile_reset();
                    continue;
                }

                if (dict_define(g_defname, body)) {
                    if (g_def_immediate && g_last_def_idx >= 0)
                        g_dict[g_last_def_idx].immediate = true;
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

            /* Immediate dispatch — the heart of self-extension. Default is to
             * COMPILE the token (append its text). But `immediate` and any word
             * flagged immediate EXECUTE now, at compile time, and may emit into
             * this very definition (postpone / `,`). Everything else — numbers,
             * primitives, ordinary words, control words, postpone — is compiled
             * and runs later. */
            {
                int32_t di = dict_find(tok);
                bool run_now = (strcmp(tok, "immediate") == 0) ||
                               (di >= 0 && g_dict[di].immediate);
                if (run_now) {
                    execute_token(tok, depth);       /* execute at compile time */
                } else if (!compile_append(tok)) {
                    compile_reset();
                }
            }
            continue;
        }

        /* --- interpreting ------------------------------------------------- */
        if (strcmp(tok, ":") == 0) {
            g_compiling     = true;
            g_need_name     = true;
            g_def_immediate = false;
            g_defname[0]    = '\0';
            continue;
        }

        if (strcmp(tok, ";") == 0) {
            err("`;` outside a definition");
            continue;
        }

        /* Defining words: `variable name` and `<value> constant name`. Both
         * consume the NEXT token as the name and add a numeric-literal word (an
         * address, or the value). Interpret-mode only — inside a body they fall
         * through to execute_token and read as an unknown word, which recovers. */
        if (strcmp(tok, "variable") == 0) {
            char name[NAME_MAX];
            const char* ns = next_token(s, name, sizeof(name));
            if (!ns) { err("`variable` needs a name"); continue; }
            s = ns;                              /* never assign NULL back to s */
            if (!defname_ok(name)) continue;
            if (!g_arena) {
                g_arena = (int32_t*)malloc((size_t)VARS_MAX * sizeof(int32_t));
                if (!g_arena) { err("out of memory for variables"); continue; }
            }
            if (g_arena_n >= VARS_MAX) { err("too many variables"); continue; }
            int32_t* cell = &g_arena[g_arena_n++];
            *cell = 0;                                  /* variables start at 0 */
            define_literal_word(name, (int32_t)(uint32_t)cell);
            continue;
        }
        if (strcmp(tok, "constant") == 0) {
            int32_t v;
            if (!pop(&v)) continue;                     /* underflow: err+reset */
            char name[NAME_MAX];
            const char* ns = next_token(s, name, sizeof(name));
            if (!ns) { err("`constant` needs a name"); continue; }
            s = ns;
            if (!defname_ok(name)) continue;
            define_literal_word(name, v);
            continue;
        }

        /* `postpone` and `,` only make sense while compiling an immediate word.
         * At the top level, report and recover (consume postpone's target so it
         * is not then run as a stray word). `,` reaches prim_comma, which also
         * errors when not compiling. */
        if (strcmp(tok, "postpone") == 0) {
            char nm[NAME_MAX];
            const char* ns = next_token(s, nm, sizeof(nm));
            if (ns) s = ns;                             /* discard the target */
            err("postpone: only inside a definition");
            continue;
        }

        /* Control words are compile-only: outside a `: ... ;` body there is no
         * structure for them to belong to. Report and discard the line. */
        if (is_ctrl_word(tok)) {
            print("  ?? ");
            print(tok);
            print(" is compile-only (use inside : ... ;)\n");
            g_abort = true;
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
