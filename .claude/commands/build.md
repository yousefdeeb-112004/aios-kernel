---
description: Build the AIOS kernel and report any compile/link errors
allowed-tools: Bash(make), Bash(make clean), Bash(make clean *)
---
Build the kernel by running `make` from the project root.

- If the build succeeds, confirm it and report the final `build/aios-kernel.bin`
  size from the make output. Do **not** launch QEMU.
- If it fails, group the errors/warnings by source file, quote the key lines, and
  propose concrete fixes. Remember the project is freestanding `-m32` with no libc
  (see CLAUDE.md) — do not suggest adding standard headers or libc calls.
- If the user passed `$ARGUMENTS`, treat it as extra context (e.g. "after my driver
  change") when interpreting the result.
