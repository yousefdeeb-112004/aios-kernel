# AIOS Kernel

A hobby **32-bit x86 (i386)** operating system kernel written in freestanding C +
GAS assembly. Boots via Multiboot under QEMU. Includes VGA/serial/keyboard/mouse/
ATA/RTC drivers, PMM/VMM/heap, processes + preemptive scheduling, Ring-3 user mode,
ELF loading, syscalls, a RAM VFS, a persistent on-disk FS (AIOS-FS), PCI + RTL8139
networking (ARP/IP/UDP/DHCP/DNS/TCP), SMP (multi-core), IPC pipes, a window manager,
and an Arabic-rooted interactive shell.

## Build & Run

| Command      | What it does                                              |
|--------------|-----------------------------------------------------------|
| `make`       | Build everything → `build/aios-kernel.bin`                |
| `make run`   | Build + boot in QEMU (`qemu-system-i386`, 128M, 4 CPUs)   |
| `make clean` | Remove `build/`                                           |

`make run` boots an **interactive** kernel (shell reads the PS/2 keyboard on the VGA
screen) and never returns. For automated/headless boot verification, run QEMU with
`-display none -serial stdio` under `timeout` and inspect the captured serial log —
see `/run` and `/debug`, and the `kernel-debugger` agent. To **see the VGA screen**
(boot banner or a shell command's output, which never reach serial), use
`/screenshot [cmd]` or `.claude/scripts/aios-screenshot.sh OUT.png "<cmd>"`.

Prefer `-drive file=disk.img,format=raw` over `-hda disk.img` in QEMU invocations —
the latter logs a spurious "Image format was not specified" warning.

Build is a 3-stage Makefile: (1) assemble + link the user program `src/user/hello.S`
into an ELF, (2) `scripts/elf2c.py` converts that ELF to a C byte array embedded in
the kernel image, (3) compile + link the kernel. `vfs.c` depends on the generated
ELF data.

## Toolchain & flags (do not "fix" these away)

- 32-bit only: `-m32`, linked with `ld -m elf_i386 -T linker.ld`.
- Freestanding, **no libc / no standard headers**: `-ffreestanding -fno-builtin
  -nostdlib -nostdinc -fno-stack-protector -mno-red-zone`. Use the in-tree
  `src/lib/string.c` (`memcpy`, `memset`, `strcmp`, `strncmp`, `strlen`, …) and
  `src/lib/kprintf.c`. There is **no** `stdio.h`, `stdlib.h`, `printf`, or `malloc`
  in user-of-libc sense — kernel allocation is `kmalloc`/`kfree` (heap.c).
- Several `implicit-*` warnings are deliberately demoted from errors. Still, prototype
  everything in the matching `include/**/*.h`.
- `-DAI_FEATURES` is on: AI event-bus / agent code is compiled in.

## Directory layout

```
src/boot/             boot.S, gdt.S            — Multiboot entry, early GDT
src/kernel/core/      kmain.c, log.c, panic.c, boot_info.c
src/kernel/arch/x86_64/  cpu, idt, pic, gdt, isr.S   (note: actually i386)
src/kernel/drivers/   vga, serial, timer(pit), keyboard, mouse, ata, rtc
src/kernel/mm/        pmm.c (phys), vmm.c (paging), heap.c (kmalloc)
src/kernel/proc/      process.c, usermode.c, elf_loader.c, *.S (context switch)
src/kernel/syscall/   syscall.c            — int 0x80 dispatch
src/kernel/fs/        vfs.c (RAM), aios_fs.c (on-disk), editor.c
src/kernel/net/       pci.c, net.c         — RTL8139 + TCP/IP stack
src/kernel/smp/       smp.c, ap_trampoline.S
src/kernel/ipc/       pipe.c
src/kernel/wm/        wm.c                 — window manager
src/kernel/ai/        event_bus.c, agent/agent.c
src/kernel/devtrack/  devtrack.c, dev/dev.c
src/kernel/shell/     shell.c              — the interactive shell (1700+ lines)
src/lib/              string.c, kprintf.c
src/user/             hello.S, user.ld     — Ring-3 demo program
include/              mirrors src/ layout (kernel/, drivers/, lib/, ai/)
```

## Boot sequence (`kmain` in `src/kernel/core/kmain.c`)

`vga_init` → `boot_info` → serial/log/cpu/pic/idt/gdt+TSS → `pmm/vmm/heap` →
`pit/keyboard/mouse/ata/rtc` + `sti` → `proc_init/syscall_init` + preemption →
`vfs_init` → AI init → devtrack → `pci/net` (auto-DHCP) → `smp_init` → AIOS-FS
auto-load → `dev/pipe` → `shell_run()` (never returns).

Anything new that needs one-time setup gets a `*_init()` call wired into `kmain`
at the right phase.

## Conventions

- **Output to screen:** `vga_puts`, `vga_putchar`, `vga_puts_color(s, fg, bg)`,
  `vga_put_dec(n)`. Shell code uses the `theme_*` color vars (`theme_header`,
  `theme_error`, `theme_value`, `theme_output`, `theme_prompt`).
- **Logging (serial/log buffer):** `LOG_INFO_MSG("TAG","msg")`, plus `WARN/ERROR/
  DEBUG`. Use short uppercase subsystem tags ("KERN", "NET", "FS").
- **Panic:** `kpanic("msg")` for unrecoverable kernel errors; CPU faults route to
  `kpanic_exception(regs)` (full register + stack dump).
- **Global state** is exposed as `g_<thing>` structs (`g_pmm_stats`, `g_net`,
  `g_smp`, `g_ata_disk`, `g_syscall_stats`, …). Follow that naming.
- **IRQ handlers** have signature `static void h(registers_t* r)`, are registered
  with `idt_register_handler(32 + irq, h)`, unmasked with `pic_unmask_irq(irq)`,
  and must `pic_send_eoi(irq)` before returning. (Vectors: 32=PIT, 33=keyboard,
  44=mouse, 14=page-fault, 128=syscall.)
- **Every new `.c` file must be added to `C_SOURCES` (or `ASM_SOURCES`) in the
  `Makefile`** — there is no globbing. Forgetting this is the #1 "why didn't my
  code run" bug.

## Common tasks → use the matching skill

- Add a shell command → skill **add-shell-command**
- Add a syscall → skill **add-syscall**
- Add a device driver → skill **add-driver**

## Gotchas

- The shell runs on **VGA + PS/2 keyboard**, not serial. Serial (`-serial stdio`)
  carries the kernel **log**, useful for headless boot/crash diagnosis.
- A triple fault reboots the CPU; `make run` already passes `-no-reboot
  -no-shutdown` so a fault halts QEMU instead of looping — read the last serial
  lines / VGA panic screen.
- Files in the RAM VFS are capped (see `VFS_FILE_MAXSZ`, `VFS_MAX_FILES` in
  `include/kernel/vfs.h`). AIOS-FS persistence requires `-hda disk.img` (the
  Makefile creates a 1 MB `disk.img` automatically) and formatting with the
  `tnsyq` shell command.
- The shell command names are transliterated Arabic roots (`sd`=help, `db`=list,
  `rq`=read, `ktb`=write, `zk`=memory, `am`=processes…). `cmd_help()` is the source
  of truth for the full list.
