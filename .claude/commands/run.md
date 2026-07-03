---
description: Build and boot the kernel headless in QEMU to verify it boots cleanly
allowed-tools: Bash(make), Bash(timeout *), Bash(qemu-system-i386 *), Read
---
Build, then do a **headless boot test** of the kernel and report whether it came up
cleanly. `$ARGUMENTS` (if given) is what the user wants to verify after boot.

1. Run `make`. Stop and report if the build fails.
2. Boot QEMU headless, capturing the kernel serial log, bounded by a timeout so it
   can't hang the session (the shell normally never exits):

   ```
   timeout 8 qemu-system-i386 -kernel build/aios-kernel.bin -m 128M \
     -no-reboot -no-shutdown -serial stdio -display none \
     -drive file=disk.img,format=raw \
     -device rtl8139,netdev=net0 -netdev user,id=net0 -smp 4 \
     > /tmp/aios-boot.log 2>&1 || true
   ```

   (Use `-drive file=disk.img,format=raw` rather than `-hda disk.img` — the latter
   prints a noisy "Image format was not specified" warning into the log every boot.)

3. Read `/tmp/aios-boot.log` and report:
   - Did it reach the shell / finish init, or did it panic / triple-fault / hang?
   - Any `PANIC`, `WARN`, or `ERROR` lines, with the surrounding context.
   - The phase progression (P1…P9, Tier 2/3, SMP, FS, DEV).

Note: the interactive shell runs on **VGA**, not serial, so the serial log shows
kernel **logging** only — that's exactly what you want for a boot/crash check. To
actually **see** the VGA screen (boot banner or a shell command's output), use the
`/screenshot` command or `.claude/scripts/aios-screenshot.sh` — it boots headless,
optionally types a command, and saves a PNG you can open. To drive the shell live,
tell the user to run `make run` in their own terminal (suggest typing `!make run`).
If a panic/hang shows up, hand off to the `kernel-debugger` agent.
