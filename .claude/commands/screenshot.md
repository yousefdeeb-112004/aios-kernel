---
description: Capture the kernel's VGA screen as a PNG (optionally after typing a shell command)
allowed-tools: Bash(make), Bash(.claude/scripts/aios-screenshot.sh *), Read
---
Capture what the kernel actually shows on the VGA screen — the only way to observe
the shell, since it doesn't render to serial.

`$ARGUMENTS` is an optional shell command to type before capturing (e.g. `nz`, `zk`,
`db`, `sd`). Only lowercase letters, digits, and spaces are supported by the key
sender.

1. Ensure the kernel is built (`make` if `build/aios-kernel.bin` is missing).
2. Run the helper (it boots headless, types the command if given, screendumps, and
   converts to PNG):

   ```
   .claude/scripts/aios-screenshot.sh /tmp/aios-screen.png "$ARGUMENTS"
   ```

3. **Read** `/tmp/aios-screen.png` to view it, and describe what's on screen:
   whether boot completed, the command's output, and anything anomalous (panic text,
   garbled glyphs, wrong values).

The helper cleans up its own QEMU process on exit. If you need several shots, give
each a distinct output path.
