---
name: add-driver
description: >
  Add a new device driver to the AIOS kernel (PS/2, PIC-IRQ, port-I/O, or PCI
  device). Use when asked to add or wire up a driver, handle a new IRQ, or talk to
  hardware via inb/outb in src/kernel/drivers. Covers IRQ registration, the Makefile
  wiring, and the kmain init hook.
---

# Add an AIOS device driver

Model new drivers on `src/kernel/drivers/keyboard/keyboard.c` (interrupt-driven) or
`pit.c` (timer). Four edits, and the **Makefile + kmain** ones are the easy-to-forget
parts.

## Step 1 — Create the files

```
src/kernel/drivers/<name>/<name>.c
include/drivers/<name>.h
```

Header declares the public API and any `g_<name>_*` global/stats struct:

```c
#ifndef _DRIVERS_FOO_H
#define _DRIVERS_FOO_H
#include <kernel/types.h>
typedef struct { uint32_t irq_count; uint8_t last; } foo_stats_t;
extern foo_stats_t g_foo_stats;
void foo_init(void);
/* ...read/write accessors... */
#endif
```

## Step 2 — Implement init + IRQ handler

Port I/O is `inb`/`outb`/`inw`/`outw` from `<kernel/ports.h>`. For an interrupt-driven
device, the handler signature is `static void h(registers_t* r)`:

```c
#include <drivers/foo.h>
#include <kernel/idt.h>
#include <kernel/pic.h>
#include <kernel/ports.h>

foo_stats_t g_foo_stats;

static void foo_irq(registers_t* r) {
    (void)r;
    uint8_t data = inb(FOO_DATA_PORT);
    g_foo_stats.irq_count++;
    g_foo_stats.last = data;
    /* ... handle ... */
    pic_send_eoi(FOO_IRQ);     /* REQUIRED — or IRQs stop firing (looks like a hang) */
}

void foo_init(void) {
    g_foo_stats.irq_count = 0;
    /* ... program the device via outb ... */
    idt_register_handler(32 + FOO_IRQ, foo_irq);  /* vector = 32 + IRQ line */
    pic_unmask_irq(FOO_IRQ);
}
```

IRQ lines already in use: 0=PIT, 1=keyboard, 12=mouse (vector 44), plus the NIC's
line. Page-fault is vector 14, syscall is 128. For a **polled** (no-IRQ) device, skip
`idt_register_handler`/`pic_unmask_irq` and just expose read/write functions.

For a **PCI** device, enumerate it through `src/kernel/net/pci.c` (see how `net.c`
finds the RTL8139 and reads its IRQ line and BARs) rather than hardcoding ports.

## Step 3 — Add to the Makefile (do not skip)

Append the `.c` to `C_SOURCES` in the `Makefile` (assembly goes in `ASM_SOURCES`).
There is no globbing — a file not listed here is silently never compiled or linked,
so `foo_init()` never runs and the driver appears to "do nothing."

```make
C_SOURCES = ... \
            src/kernel/drivers/foo/foo.c \
            ...
```

## Step 4 — Initialize it in `kmain` (`src/kernel/core/kmain.c`)

Add `#include <drivers/foo.h>` and call `foo_init()` in the right boot phase —
alongside the other driver inits (after `idt_init`/`pic_init`, near `keyboard_init();
mouse_init();`), and **before** `__asm__ volatile("sti")` if it must be ready when
interrupts are enabled. Add a one-line `LOG_INFO_MSG("FOO", "ready")` to confirm it
ran.

## Verify

`make`, then boot-test (`/run`) and look for your log line in the serial output. If
the device is interrupt-driven and the kernel hangs right after enabling it, the
prime suspect is a missing `pic_send_eoi`. If nothing happens at all, re-check Step 3
(Makefile) and Step 4 (init call).
