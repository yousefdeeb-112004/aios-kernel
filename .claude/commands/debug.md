---
description: Diagnose a kernel crash, panic, triple fault, or boot hang
allowed-tools: Bash(make), Bash(timeout *), Bash(qemu-system-i386 *), Read, Grep, Glob
---
The kernel is crashing, panicking, hanging, or rebooting. Diagnose the root cause.

Context from the user (symptom, recent change, the command that triggers it):
$ARGUMENTS

Delegate the investigation to the **kernel-debugger** subagent, which knows this
project's fault model (page faults via IDT vector 14, GPF, triple-fault reboot
loops), the serial-log capture workflow, and how to correlate a faulting EIP with
the linker map / disassembly. Give it the symptom above plus any panic text the user
pasted, and have it return the most likely cause and a minimal fix.
