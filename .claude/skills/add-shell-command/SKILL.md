---
name: add-shell-command
description: >
  Add a new command to the AIOS interactive shell. Use when asked to add, create, or
  wire up a shell command / built-in (the transliterated-Arabic commands like sd, db,
  rq, ktb) in src/kernel/shell/shell.c, or when a new command "isn't recognized".
---

# Add an AIOS shell command

All shell logic lives in `src/kernel/shell/shell.c`. A command is three edits in that
one file — no Makefile change is needed (shell.c is already compiled).

## Naming

Commands are short transliterated Arabic roots (`sd`=help, `db`=list, `rq`=read,
`ktb`=write, `zk`=memory, `am`=processes, `wqf`=halt…). Pick a short lowercase
mnemonic that isn't already taken. `cmd_help()` is the canonical list — grep it and
the `shell_exec` chain to avoid collisions.

## Step 1 — Write the handler

Add a `static void cmd_<name>(...)` near the other `cmd_*` functions. Use the project
output helpers and the `theme_*` colors (do **not** use printf):

```c
/* No-argument command */
static void cmd_foo(void) {
    vga_puts_color("=== Foo (foo) ===\n", theme_header, VGA_BLACK);
    vga_puts_color("hello from foo\n", theme_output, VGA_BLACK);
}

/* Command that takes an argument string */
static void cmd_bar(const char* arg) {
    arg = skip_spaces(arg);
    if (!arg[0]) {
        vga_puts_color("Usage: bar <thing>\n", theme_error, VGA_BLACK);
        return;
    }
    vga_puts("  bar got: ");
    vga_puts_color(arg, theme_value, VGA_BLACK);
    vga_putchar('\n');
}
```

Output helpers: `vga_puts`, `vga_putchar`, `vga_puts_color(s, fg, bg)`,
`vga_put_dec(n)`. Need a forward declaration only if you call it before its
definition.

## Step 2 — Dispatch it in `shell_exec()`

`shell_exec()` (near the bottom of shell.c) is a long `if / else if` chain on the
command word. Add a branch **before the final `else`** that prints "unknown command".

```c
/* No-arg command: exact match */
} else if (strcmp(cmd, "foo") == 0) {
    cmd_foo();

/* Command with an argument: match the word, require space or end-of-string,
   then pass the rest. The literal length (3 here) MUST equal strlen("bar"). */
} else if (strncmp(cmd, "bar", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
    cmd_bar(cmd + 3);
```

The `(cmd[N] == ' ' || cmd[N] == '\0')` guard prevents `bar` from also matching
`barbaz`. `N` and the `strncmp` length must both equal the command's character count.

## Step 3 — List it in `cmd_help()`

Add a line to `cmd_help()` so users can discover it. Match the existing formatting
(command, short description). If you bumped the advertised command count, the banner
string in the `Makefile`'s `all:` rule and the version line are nice-to-update but
optional.

## Verify

`make` to confirm it compiles, then boot-test (`/run`) — or have the user `!make run`
and type the new command at the `aios>` prompt. Common mistakes: forgetting Step 2
(command "not recognized"), a `strncmp` length that doesn't match the word, or
omitting the space/null guard so it shadows another command.
