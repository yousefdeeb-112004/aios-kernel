/* AIOS Kernel v1.1.0 — All 9 Phases + AIOS Shell (Tier 1) */
#include <kernel/types.h>
#include <kernel/boot_info.h>
#include <kernel/multiboot.h>
#include <kernel/cpu.h>
#include <kernel/idt.h>
#include <kernel/gdt.h>
#include <kernel/pic.h>
#include <kernel/log.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <kernel/devtrack.h>
#include <kernel/shell.h>
#include <ai/event_bus.h>
#include <ai/agent.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <drivers/pit.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/ata.h>
#include <drivers/rtc.h>
#include <kernel/pci.h>
#include <kernel/net.h>
#include <kernel/smp.h>
#include <kernel/smp.h>
#include <kernel/aios_fs.h>
#include <kernel/dev.h>
#include <kernel/pipe.h>
#include <lib/string.h>
#include <lib/kprintf.h>

void kmain(uint32_t magic, uint32_t mbi) {
    /* === P1: Boot === */
    vga_init();
    vga_puts_color("    _    ___ ___  ____    _  __                    _\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_color("   / \\  |_ _/ _ \\/ ___|  | |/ /___ _ __ _ __   ___| |\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_color("  / _ \\  | | | | \\___ \\  | ' // _ \\ '__| '_ \\ / _ \\ |\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_color(" / ___ \\ | | |_| |___) | | . \\  __/ |  | | | |  __/ |\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_color("/_/   \\_\\___\\___/|____/  |_|\\_\\___|_|  |_| |_|\\___|_|\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("\n");
    vga_puts_color("AIOS Kernel v2.0.0", VGA_WHITE, VGA_BLACK);
    vga_puts(" + ");
    vga_puts_color("Shell", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(" + ");
    vga_puts_color("User Mode", VGA_LIGHT_MAGENTA, VGA_BLACK);
    vga_puts(" + ");
    vga_puts_color("ELF", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts(" + ");
    vga_puts_color("Disk", VGA_LIGHT_BLUE, VGA_BLACK);
    vga_puts("\n\n");
    boot_info_init(magic, (void*)mbi);
    boot_info_dump();

    /* === P2: Early Init === */
    serial_init(); log_init(); cpu_detect(); cpu_dump();
    pic_init(); idt_init();
    gdt_init_full();  /* Replace boot GDT with full GDT + TSS for Ring 3 */
    LOG_INFO_MSG("KERN", "P2: Serial CPU PIC IDT GDT+TSS");

    /* === P4: Memory === */
    multiboot_info_t* mb = (multiboot_info_t*)mbi;
    if (magic == MULTIBOOT_MAGIC && (mb->flags & MULTIBOOT_FLAG_MMAP))
        pmm_init(mb->mmap_addr, mb->mmap_length, g_boot_info.kernel_start, g_boot_info.kernel_end);
    vmm_init(); heap_init();
    LOG_INFO_MSG("KERN", "P4: PMM VMM Heap");

    /* === P3: Interrupts + Drivers === */
    pit_init(); keyboard_init(); mouse_init();
    ata_init(); rtc_init();
    __asm__ volatile("sti");
    LOG_INFO_MSG("KERN", "P3: Timer KB Mouse ATA RTC ON");

    /* Show RTC time */
    {
        char tbuf[24];
        rtc_format(&g_rtc_time, tbuf, sizeof(tbuf));
        vga_puts("  RTC: ");
        vga_puts_color(tbuf, VGA_WHITE, VGA_BLACK);
        vga_puts("\n");
    }
    if (g_ata_disk.present) {
        vga_puts("  Disk: ");
        vga_puts_color(g_ata_disk.model, VGA_WHITE, VGA_BLACK);
        vga_puts(" ");
        vga_put_dec(g_ata_disk.size_mb);
        vga_puts("MB\n");
    } else {
        vga_puts_color("  Disk: not attached (use -hda disk.img)\n", VGA_DARK_GREY, VGA_BLACK);
    }

    /* === P5+P6: Processes + Syscalls === */
    proc_init(); syscall_init();
    proc_enable_preemption();
    LOG_INFO_MSG("KERN", "P5+P6: Procs + Syscalls");

    /* === P7: Filesystem === */
    vfs_init();
    LOG_INFO_MSG("VFS", "P7: RAM disk ready");

    /* === P8: AI === */
    ai_init(); ai_sample_perf();
    ai_agent_init();
    LOG_INFO_MSG("AI", "P8: Event bus + AI Agent active");

    /* === P9: Dev Tracking === */
    devtrack_init();
    LOG_INFO_MSG("DEV", "P9: Tracking active");

    /* === Tier 3: PCI + Networking === */
    pci_init();
    net_init();
    if (g_net.up) {
        vga_puts("  NIC: ");
        for (int i = 0; i < 6; i++) {
            const char hex[] = "0123456789ABCDEF";
            vga_putchar(hex[g_net.mac.b[i] >> 4]);
            vga_putchar(hex[g_net.mac.b[i] & 0xF]);
            if (i < 5) vga_putchar(':');
        }
        vga_puts("\n");
        LOG_INFO_MSG("NET", "Network UP (RTL8139)");
        /* Auto-DHCP: get real IP at boot */
        net_dhcp_request();
    } else {
        vga_puts_color("  NET: no NIC (use -nic model=rtl8139)\n", VGA_DARK_GREY, VGA_BLACK);
    }

    /* === Tier 3: SMP (Multi-Core) === */
    smp_init();
    vga_puts("  SMP: "); vga_put_dec(g_smp.cpu_count); vga_puts(" CPUs, ");
    vga_put_dec(g_smp.cpus_online); vga_puts(" online");
    if (g_smp.smp_enabled)
        vga_puts_color(" [Multi-Core]\n", VGA_LIGHT_GREEN, VGA_BLACK);
    else
        vga_puts_color(" [Single-Core]\n", VGA_YELLOW, VGA_BLACK);
    LOG_INFO_MSG("SMP", "Multi-core initialized");

    /* === AIOS-FS: Persistent filesystem auto-load === */
    if (g_ata_disk.present && aiosfs_detect()) {
        aiosfs_load();
        vga_puts_color("  FS: AIOS-FS loaded from disk\n", VGA_LIGHT_GREEN, VGA_BLACK);
        LOG_INFO_MSG("FS", "AIOS-FS loaded from disk");
    } else if (g_ata_disk.present) {
        vga_puts_color("  FS: Disk not formatted (use 'tnsyq')\n", VGA_DARK_GREY, VGA_BLACK);
    }

    /* === Device Layer === */
    dev_init();
    pipe_init();
    vga_puts("  DEV: ");
    vga_put_dec(g_dev_count);
    vga_puts(" devices, IPC pipes ready\n");
    LOG_INFO_MSG("DEV", "Device layer + IPC initialized");

    /* Phase status */
    vga_puts_color("All phases + Tier 2+3 ", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts_color("[OK]\n", VGA_LIGHT_GREEN, VGA_BLACK);

    /* === Launch Shell === */
    shell_init();
    shell_run();  /* Never returns */
}
