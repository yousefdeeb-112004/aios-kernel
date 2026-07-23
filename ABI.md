# AIOS System Call ABI — v1.2

This document is the frozen contract between Ring 3 user programs and the AIOS
kernel. Every entry below is verified against the source:
`include/kernel/syscall.h`, `src/kernel/syscall/syscall.c`,
`src/kernel/proc/process.c`, `src/kernel/fs/vfs.c`, and
`src/kernel/proc/elf_loader.c`.

> **Stability:** These numbers and conventions are frozen. New syscalls append
> (taking the next free number); existing numbers never change meaning.
> `SYS_MAX` bounds the table.

### Changelog

- **v1.2** — added `SYS_AISTAT` (14): a read-only snapshot of the kernel AI
  agent's Bayesian nodes into a user `ai_stat_t`. Numbers 1–13 are unchanged.
  `ai_stat_t` is itself **versioned and append-only**: it carries a `version`
  field (currently 1), and future revisions may only **add fields at the end**
  and bump the version — existing field offsets and meanings never change, so
  an old program keeps reading the prefix it knows.
- **v1.1** — added `SYS_OPEN` (11), `SYS_FREAD` (12), `SYS_FCLOSE` (13):
  read-only VFS file access for Ring 3. `EDX` is now a real third argument
  (it was reserved in v1). Numbers 1–10 are unchanged.
- **v1** — initial frozen ABI, syscalls 1–10.

## Invocation

- Trap instruction: **`int $0x80`** (IDT vector 128, handler `sh` in
  `syscall.c`).
- **Syscall number → `EAX`.**
- **Argument 1 → `EBX`** (`a1`), **argument 2 → `ECX`** (`a2`),
  **argument 3 → `EDX`** (`a3`).
- `EDX` was *reserved* in v1 and became a real third argument in **v1.1**
  (`SYS_FREAD` is the first user). Syscalls that take fewer than three
  arguments ignore it, so passing garbage in `EDX` to a one- or two-argument
  syscall is harmless — v1 programs stay source- and binary-compatible. The
  ISR stub's `pushal`/`popal` already saved and restored `EDX`, and
  `registers_t` lays it out in the matching slot, so no stub change was
  needed.
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
| 7 | `SYS_SBRK` | `int32_t increment` | — | previous program break as a pointer, or `(void*)-1` on failure. See **sbrk semantics** below. |
| 8 | `SYS_KILL` | `uint32_t pid` | `int signum` | `0` on success; negative on error (`-1` bad signum, `-2` no such pid, `-3` illegal signal to pid 0). |
| 9 | `SYS_SIGNAL` | `int signum` | `void (*handler)(int)` | `0` on success; negative on error. `handler` may be `SIG_DFL` (0) or `SIG_IGN` (1). |
| 10 | `SYS_GETSTATS` | `system_stats_t* out` | — | `0` on success; `-1` if the pointer is NULL/invalid. Fills `out`. |
| 11 | `SYS_OPEN` *(v1.1)* | `const char* path` | — | fd `>= 3` on success. `-1` bad path pointer, `-2` no such file (or it is a directory), `-3` the process's fd table is full. **Read-only.** |
| 12 | `SYS_FREAD` *(v1.1)* | `int fd` | `char* buf` | bytes read, **`0` at EOF**. `-1` bad buffer, `-2` bad fd. Third argument `uint32_t len` in **`EDX`**. |
| 13 | `SYS_FCLOSE` *(v1.1)* | `int fd` | — | `0` on success; `-2` bad fd. |
| 14 | `SYS_AISTAT` *(v1.2)* | `ai_stat_t* out` | — | `0` on success; `-1` if the pointer is NULL/out-of-region. Fills `out`. **Read-only** — snapshots agent state, never mutates it. |

`SYS_MAX` = 15 (one past the last valid number). Any `EAX` ≥ `SYS_MAX` or
otherwise unhandled returns `-1` and increments the invalid-syscall counter.

`system_stats_t` (from `include/kernel/syscall.h`) is eight `uint32_t` fields:
`uptime_seconds, total_ticks, free_pages, used_pages, heap_allocated,
active_processes, total_ctx_switches, total_syscalls`.

`ai_stat_t` (from `include/kernel/syscall.h`, mirrored manually in
`src/user/lib/syscalls.h`) is a **frozen, versioned** struct — ten `uint32_t`
fields at v1: `version` (= `AI_STAT_VERSION`, currently 1); then for node 1
(memory-leak) `n1_alpha, n1_beta, n1_permille, n1_run_e`; for node 2
(syscall-spike) `n2_alpha, n2_beta, n2_permille, n2_run_e`; and `anomalies`, a
bitmask where bit *i* is set iff anomaly *i* (`anomaly_type_t`) is currently
active. `permille` is the posterior mean `alpha*1000/(alpha+beta)` (integer;
no floats cross the ABI). New fields only ever **append** and bump `version`.
`SYS_AISTAT` validates `out` with the same `user_range_ok` gate as
`SYS_GETSTATS`, so a kernel or out-of-region pointer returns `-1`.

## Pointer rules (user-mode)

Every syscall argument that is a pointer into user memory is validated **when the
caller is Ring 3** (decided by the saved CS privilege level, `cs & 3 == 3`):

- The whole byte range **`[ptr, ptr + len)` must lie inside
  `[USER_REGION_START, USER_STACK_TOP)`** — i.e. `[0x00500000, 0x00600000)`.
- The bound check is overflow-safe; an empty range (`len == 0`) is accepted.
- On violation the syscall **returns `-1` and does nothing** — it does **not**
  kill the process. (Direct dereferences of out-of-region memory by user code
  are handled separately by the page-fault path, which kills only that process.)

Validated buffer arguments: `SYS_WRITE` (buf), `SYS_READ` (buf), `SYS_GETSTATS`
(out), `SYS_SIGNAL` (handler entry point, unless it is the `SIG_DFL`/`SIG_IGN`
sentinel), **`SYS_FREAD` (buf, checked against the `EDX` length)**, and
**`SYS_AISTAT` (out, checked for `sizeof(ai_stat_t)`)**. The
region constants `USER_REGION_START` and `USER_STACK_TOP` are defined in
`include/kernel/usermode.h` and shared by the ELF loader and the syscall
validator.

### String arguments (v1.1)

`SYS_OPEN`'s `path` is a NUL-terminated **string**, not a counted buffer, so the
range check above does not apply: the kernel does not know the length until it
has already read the bytes. `user_str_ok(ptr, maxlen)` handles this:

- `ptr` must satisfy `USER_REGION_START <= ptr < USER_STACK_TOP`.
- The scan is clamped to `min(maxlen, USER_STACK_TOP - ptr)` bytes, so it
  **can never read at or above `USER_STACK_TOP`** — the validator itself cannot
  be made to walk off the end of the user region into kernel memory.
- `maxlen` is `VFS_MAX_NAME` (64), the VFS's own name limit.
- A string with **no NUL inside that window is rejected** (`-1`), not silently
  truncated.

The path is then **copied into a kernel-side buffer before the VFS sees it**.
Validating a user pointer and then dereferencing it repeatedly would be a TOCTOU
bug: another thread sharing the address space could rewrite the path between the
check and the `find_file()` lookup. The snapshot removes that window entirely.

### Read-only scope (v1.1)

`SYS_OPEN` calls `vfs_open()`, which is hard-wired to `VFS_MODE_READ`. There is
deliberately **no `SYS_FWRITE`, `SYS_CREATE`, `SYS_DELETE`, `SYS_SEEK` or
`SYS_STAT`** — Ring 3 can read file contents and nothing else. It cannot create,
modify, truncate, rename or delete anything in the VFS, and it cannot reposition
a descriptor (reads advance the offset sequentially from 0 until EOF).

### File descriptors (v1.1)

`SYS_OPEN` returns an index into the **calling process's own** fd table
(`process_t.fds`, `PROC_MAX_FDS` = 16). Ownership is *structural*: the table
lives in the caller's PCB, so a process cannot name another process's
descriptor — there is no global fd namespace exposed to Ring 3. On top of that,
`SYS_FREAD`/`SYS_FCLOSE` re-validate every fd via `proc_fd_target()`: in range,
actually open, and of type `FD_FILE`. That last check is what stops Ring 3 from
handing fd 0/1/2 (the `FD_CONSOLE` stdio slots) to a file syscall. User fds
therefore always start at **3**.

Descriptors are **reclaimed at process exit**. The per-process table is reset
when a PCB slot is reused, but the VFS descriptor it referenced is a *global*
slot (`VFS_MAX_OPEN` = 32 system-wide), so a program that exits without
`SYS_FCLOSE` would pin one forever. `proc_release_fds()` — called from the
process reaper and from `proc_kill()` — closes any still-open `FD_FILE` entry,
so leaked descriptors cannot exhaust the VFS.

**Kernel-mode callers** (Ring 0 kernel threads using the `sys_*` wrappers at the
bottom of `syscall.c`) pass trusted pointers and are **not** range- or
string-checked; their behavior is unchanged.

## User memory layout (Ring 3)

Set up by the ELF loader (`src/kernel/proc/elf_loader.c`, linker script
`src/user/user.ld`). Each user program runs in a private address space; the
kernel is cloned in as supervisor-only, so Ring 3 can reach only its own pages.

| Region | Address | Size | Notes |
|--------|---------|------|-------|
| Program image | `USER_REGION_START` = `0x00500000` | program's mapped PT_LOAD span, page-aligned | Load base fixed by `user.ld`. Confined to a single 4 MB page-directory slot. The loader **refuses** a segment reaching past `USER_HEAP_START`. |
| *(unused gap)* | `0x00501000` – `0x0053FFFF` | ~252 KB | Headroom for the image to grow. Unmapped. |
| **User heap** | `USER_HEAP_START` = `0x00540000` | grows up, max `USER_HEAP_MAX` = `0x005FE000` (760 KB) | Grown on demand by `SYS_SBRK`. Pages are fresh PMM frames mapped `PTE_USER` into the process's own page directory, and are **zeroed** before Ring 3 sees them. |
| **Guard page** | `0x005FE000` – `0x005FEFFF` | one 4 KB page | **Permanently unmapped.** Separates heap from stack. |
| User stack | top at `USER_STACK_TOP` = `0x00600000` | **one 4 KB page** (`[0x005FF000, 0x00600000)`) | Initial `ESP` = `USER_STACK_TOP - 16`. |

Valid user-pointer window for the ABI: **`[0x00500000, 0x00600000)`** (1 MB).
The heap constants live in `include/kernel/usermode.h` alongside the region
constants. A compile-time assertion in `process.c` verifies that the whole heap
range shares the program's single 4 MB page-directory slot — the one slot
`vmm_map_user_page` backs per address space.

### sbrk semantics

`SYS_SBRK` → `proc_sbrk()` (`src/kernel/proc/process.c`) takes two paths,
selected by whether the calling process has a **private address space**
(`page_dir != vmm_get_kernel_page_dir()`):

**Ring 3 / private address space — the real user heap.** The break starts at
`USER_HEAP_START` (anchored by the ELF loader). Growing it allocates a
`pmm_alloc_page()` frame for every page the break newly crosses and maps it
`PTE_USER` into *that process's* page directory. **The returned pointer is
genuinely dereferenceable from user mode** — this is what `shb` demonstrates.

**Kernel threads.** Unchanged: a `kmalloc`-backed per-process heap. Ring 0
callers already have access to kernel memory, so nothing here needed fixing.

Return value is the **old** break on success (POSIX), `(void*)-1` on failure.

#### Guarantees and deliberate limits

- **Upper bound.** A grow that would push the break past `USER_HEAP_MAX`
  (`0x005FE000`) returns `-1` and changes nothing. Because `USER_HEAP_MAX` is
  the base of the permanently-unmapped **guard page**, the heap can never grow
  into the user stack silently; symmetrically, a runaway stack faults into the
  guard page instead of scribbling on the heap.
- **Failure policy: fail, do not roll back.** If `pmm_alloc_page()` runs dry
  part-way through a grow, `sbrk` returns `-1` and leaves the break *unchanged*
  — but the pages it already mapped stay mapped. This leaks nothing: the
  process records how far it has mapped (`user_brk_mapped`), so a later
  successful `sbrk` reuses those pages rather than allocating the same virtual
  page twice, and every one of them is reclaimed at process exit.
- **Negative increments are a v1 simplification.** A negative `increment` moves
  the break down (clamped at `USER_HEAP_START`) and returns the old break, but
  **does not unmap or free** the pages above the new break. Memory is returned
  to the PMM only when the process exits. Partial unmapping is not implemented.
- **Frame reclamation.** Heap pages are `PTE_PRESENT | PTE_USER` entries in the
  process's private page table, which is exactly what
  `vmm_destroy_address_space()` walks and frees. Process exit (normal,
  `proc_kill`, or a page-fault kill) therefore returns every heap frame to the
  PMM with no extra bookkeeping. `shb` prints `free_pages` before and after to
  make this checkable.
- **Zeroing.** New heap pages are zeroed through their new mapping, so a
  recycled frame cannot hand another process's leftovers to Ring 3.
- **No overcommit, no `brk`.** There is no separate `SYS_BRK`, no `mmap`, and no
  demand paging — pages are allocated eagerly at `sbrk` time.
