---
name: add-syscall
description: >
  Add a new system call to the AIOS kernel (the int 0x80 interface). Use when asked
  to add a syscall, expose a kernel service to Ring-3 user programs, or extend the
  syscall dispatcher in src/kernel/syscall/syscall.c and include/kernel/syscall.h.
---

# Add an AIOS syscall

Syscalls use software interrupt **`int 0x80`** (IDT vector 128). The convention:
`eax` = syscall number, `ebx` = arg1, `ecx` = arg2, return value in `eax`. Two files
are involved: `include/kernel/syscall.h` and `src/kernel/syscall/syscall.c`. No
Makefile change is needed.

## Step 1 — Reserve a number (`include/kernel/syscall.h`)

Add a `#define` and **bump `SYS_MAX`** (it sizes the stats array and bounds-checks
the dispatcher — getting this wrong is a silent out-of-bounds):

```c
#define SYS_GETSTATS 10
#define SYS_FOO      11   /* new */
#define SYS_MAX      12   /* was 11 — must always be highest number + 1 */
```

Declare the user-mode wrapper prototype in the same header next to the others.

## Step 2 — Implement the handler (`src/kernel/syscall/syscall.c`)

Add a `case` to the `switch (n)` in `sh()`. Read args from the register frame, write
the result into `r->eax`:

```c
case SYS_FOO: {
    const char* name = (const char*)a1;   /* a1 = r->ebx */
    uint32_t     flag = a2;                /* a2 = r->ecx */
    r->eax = (uint32_t)do_foo(name, flag); /* return value */
    break;
}
```

Notes:
- `a1`/`a2` are already unpacked at the top of `sh()` from `r->ebx`/`r->ecx`.
- Pointers from user mode point into the **current process address space** — treat
  them as untrusted; validate before dereferencing.
- The dispatcher already counts stats and runs `proc_check_signals()` after the
  switch; just add your case.

## Step 3 — Add the user-mode wrapper (`src/kernel/syscall/syscall.c`)

So kernel/user C code can call the syscall without inline asm at each site. Match the
existing one-liners:

```c
int sys_foo(const char* name, uint32_t flag) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_FOO), "b"(name), "c"(flag));
    return r;
}
```

For a no-return-value call, drop the `"=a"` output. For no args, drop `"b"`/`"c"`.

## Verify

`make`, then boot-test. If a user program calls it, that program lives in
`src/user/` (assembly, linked separately and embedded via `scripts/elf2c.py`). A
syscall that returns `(uint32_t)-1` unexpectedly usually means it hit the `default`
case — recheck the number, `SYS_MAX`, and that the `case` label matches the
`#define`.
