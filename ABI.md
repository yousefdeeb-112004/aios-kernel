# AIOS System Call ABI — v1

This document is the frozen contract between Ring 3 user programs and the AIOS
kernel. Every entry below is verified against the source:
`include/kernel/syscall.h`, `src/kernel/syscall/syscall.c`,
`src/kernel/proc/process.c`, and `src/kernel/proc/elf_loader.c`.

> **Stability:** These numbers and conventions are frozen as **ABI v1**. New
> syscalls append (taking the next free number); existing numbers never change
> meaning. `SYS_MAX` bounds the table.

## Invocation

- Trap instruction: **`int $0x80`** (IDT vector 128, handler `sh` in
  `syscall.c`).
- **Syscall number → `EAX`.**
- **Argument 1 → `EBX`** (`a1`), **argument 2 → `ECX`** (`a2`).
- **`EDX` is reserved** for a future third argument; it is not read today.
- **Return value → `EAX`.** By convention **negative values indicate an error**
  (returned as `(uint32_t)-1` on failure).
- Registers other than `EAX` are preserved across the call (the ISR saves and
  restores the full GP set).
- After every syscall the kernel runs `proc_check_signals()` for the caller.

## Syscall table

| # | Name | EBX (a1) | ECX (a2) | Returns |
|---|------|----------|----------|---------|
| 1 | `SYS_WRITE` | `const char* buf` | `uint32_t len` | `len` (the requested count). Writes bytes to the VGA console, stopping early at the first NUL byte. |
| 2 | `SYS_READ` | `char* buf` | `uint32_t len` | number of bytes actually read from the keyboard (stops early when no character is available). |
| 3 | `SYS_GETPID` | — | — | current process PID (0 for the kernel process). |
| 4 | `SYS_EXIT` | — | — | does not return; terminates the calling process. |
| 5 | `SYS_YIELD` | — | — | no value; voluntarily yields the CPU to the scheduler. |
| 6 | `SYS_UPTIME` | — | — | uptime in **seconds** (`pit_get_uptime`). |
| 7 | `SYS_SBRK` | `int32_t increment` | — | previous program break as a pointer, or `(void*)-1` on failure. See **sbrk limitation** below. |
| 8 | `SYS_KILL` | `uint32_t pid` | `int signum` | `0` on success; negative on error (`-1` bad signum, `-2` no such pid, `-3` illegal signal to pid 0). |
| 9 | `SYS_SIGNAL` | `int signum` | `void (*handler)(int)` | `0` on success; negative on error. `handler` may be `SIG_DFL` (0) or `SIG_IGN` (1). |
| 10 | `SYS_GETSTATS` | `system_stats_t* out` | — | `0` on success; `-1` if the pointer is NULL/invalid. Fills `out`. |

`SYS_MAX` = 11 (one past the last valid number). Any `EAX` ≥ `SYS_MAX` or
otherwise unhandled returns `-1` and increments the invalid-syscall counter.

`system_stats_t` (from `include/kernel/syscall.h`) is eight `uint32_t` fields:
`uptime_seconds, total_ticks, free_pages, used_pages, heap_allocated,
active_processes, total_ctx_switches, total_syscalls`.

## Pointer rules (user-mode)

Every syscall argument that is a pointer into user memory is validated **when the
caller is Ring 3** (decided by the saved CS privilege level, `cs & 3 == 3`):

- The whole byte range **`[ptr, ptr + len)` must lie inside
  `[USER_REGION_START, USER_STACK_TOP)`** — i.e. `[0x00500000, 0x00600000)`.
- The bound check is overflow-safe; an empty range (`len == 0`) is accepted.
- On violation the syscall **returns `-1` and does nothing** — it does **not**
  kill the process. (Direct dereferences of out-of-region memory by user code
  are handled separately by the page-fault path, which kills only that process.)

Validated arguments: `SYS_WRITE` (buf), `SYS_READ` (buf), `SYS_GETSTATS` (out),
and `SYS_SIGNAL` (handler entry point, unless it is the `SIG_DFL`/`SIG_IGN`
sentinel). The region constants `USER_REGION_START` and `USER_STACK_TOP` are
defined in `include/kernel/usermode.h` and shared by the ELF loader and the
syscall validator.

**Kernel-mode callers** (Ring 0 kernel threads using the `sys_*` wrappers at the
bottom of `syscall.c`) pass trusted pointers and are **not** range-checked; their
behavior is unchanged.

## User memory layout (Ring 3)

Set up by the ELF loader (`src/kernel/proc/elf_loader.c`, linker script
`src/user/user.ld`). Each user program runs in a private address space; the
kernel is cloned in as supervisor-only, so Ring 3 can reach only its own pages.

| Region | Address | Size | Notes |
|--------|---------|------|-------|
| Program image | `USER_REGION_START` = `0x00500000` | program's mapped PT_LOAD span, page-aligned | Load base fixed by `user.ld`. Confined to a single 4 MB page-directory slot. |
| User stack | top at `USER_STACK_TOP` = `0x00600000` | **one 4 KB page** (`[0x005FF000, 0x00600000)`) | Initial `ESP` = `USER_STACK_TOP - 16`. |

Valid user-pointer window for the ABI: **`[0x00500000, 0x00600000)`** (1 MB).

### sbrk limitation (honest note)

`SYS_SBRK` → `proc_sbrk()` (`src/kernel/proc/process.c`) grows/shrinks a
per-process heap that is allocated with the **kernel** allocator (`kmalloc`).
The returned break pointer therefore points into **kernel heap memory**, which
is **supervisor-only and not mapped into the user address space**.

Consequence: a genuine Ring 3 program can call `SYS_SBRK` and receive a pointer,
but **dereferencing it from user mode will fault** (and, under the current
page-fault policy, kill the process). In other words, `sbrk` is functional for
kernel-thread callers but **is not usable for real user-mode heap allocation
today**. This is a known limitation, documented here rather than papered over;
fixing it would require backing the user heap with user-accessible frames mapped
into the process's address space. No behavior is changed by this document.
