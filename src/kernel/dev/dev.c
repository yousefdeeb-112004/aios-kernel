/* =============================================================================
 * Device Abstraction Layer — Implementation
 *
 * Registers built-in devices at boot. Each device is a set of function
 * pointers (read/write/ioctl) behind a unified API.
 * ============================================================================= */

#include <kernel/dev.h>
#include <kernel/lock.h>
#include <kernel/ports.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <drivers/keyboard.h>
#include <drivers/ata.h>
#include <drivers/pit.h>
#include <lib/string.h>
#include <lib/kprintf.h>

device_t g_devices[DEV_MAX];
uint32_t g_dev_count = 0;

/* =========================================================================
 * Device Registry
 * ========================================================================= */

int32_t dev_register(const char* name, dev_type_t type, dev_ops_t ops) {
    if (g_dev_count >= DEV_MAX) return -1;
    device_t* d = &g_devices[g_dev_count];
    memset(d, 0, sizeof(device_t));
    /* Safe copy name */
    uint32_t nlen = strlen(name);
    if (nlen > DEV_NAME_MAX - 1) nlen = DEV_NAME_MAX - 1;
    memcpy(d->name, name, nlen);
    d->name[nlen] = '\0';
    d->type = type;
    d->ops = ops;
    d->registered = true;
    g_dev_count++;
    return (int32_t)(g_dev_count - 1);
}

device_t* dev_find(const char* name) {
    for (uint32_t i = 0; i < g_dev_count; i++) {
        if (g_devices[i].registered && strcmp(g_devices[i].name, name) == 0)
            return &g_devices[i];
    }
    return NULL;
}

int32_t dev_read(const char* name, void* buf, uint32_t count) {
    device_t* d = dev_find(name);
    if (!d || !d->ops.read) return -1;
    int32_t r = d->ops.read(buf, count);
    if (r > 0) { d->total_reads++; d->bytes_read += r; }
    return r;
}

int32_t dev_write(const char* name, const void* buf, uint32_t count) {
    device_t* d = dev_find(name);
    if (!d || !d->ops.write) return -1;
    int32_t r = d->ops.write(buf, count);
    if (r > 0) { d->total_writes++; d->bytes_written += r; }
    return r;
}

int32_t dev_ioctl(const char* name, uint32_t cmd, uint32_t arg) {
    device_t* d = dev_find(name);
    if (!d || !d->ops.ioctl) return -1;
    return d->ops.ioctl(cmd, arg);
}

/* =========================================================================
 * Built-in Device: /dev/console (VGA + keyboard)
 * ========================================================================= */

static int32_t console_read(void* buf, uint32_t count) {
    char* p = (char*)buf;
    uint32_t rd = 0;
    for (uint32_t i = 0; i < count; i++) {
        char c = keyboard_getchar();
        if (!c) break;
        p[i] = c;
        rd++;
    }
    return (int32_t)rd;
}

static int32_t console_write(const void* buf, uint32_t count) {
    const char* p = (const char*)buf;
    for (uint32_t i = 0; i < count; i++) {
        vga_putchar(p[i]);
    }
    return (int32_t)count;
}

/* =========================================================================
 * Built-in Device: /dev/serial (COM1)
 * ========================================================================= */

static int32_t serial_dev_read(void* buf, uint32_t count) {
    /* Read from serial — check if data available */
    uint8_t* p = (uint8_t*)buf;
    uint32_t rd = 0;
    for (uint32_t i = 0; i < count; i++) {
        /* Check Line Status Register bit 0 = Data Ready */
        uint8_t lsr = inb(SERIAL_COM1 + 5);
        if (!(lsr & 0x01)) break;
        p[i] = inb(SERIAL_COM1);
        rd++;
    }
    return (int32_t)rd;
}

static int32_t serial_dev_write(const void* buf, uint32_t count) {
    const char* p = (const char*)buf;
    for (uint32_t i = 0; i < count; i++) {
        serial_putchar(p[i]);
    }
    return (int32_t)count;
}

/* =========================================================================
 * Built-in Device: /dev/kbd (raw keyboard, read-only)
 * ========================================================================= */

static int32_t kbd_read(void* buf, uint32_t count) {
    return console_read(buf, count);  /* Same as console read */
}

/* =========================================================================
 * Built-in Device: /dev/null (discards everything)
 * ========================================================================= */

static int32_t null_read(void* buf, uint32_t count) {
    (void)buf; (void)count;
    return 0;  /* EOF immediately */
}

static int32_t null_write(const void* buf, uint32_t count) {
    (void)buf;
    return (int32_t)count;  /* Accept and discard */
}

/* =========================================================================
 * Built-in Device: /dev/zero (reads return zero bytes)
 * ========================================================================= */

static int32_t zero_read(void* buf, uint32_t count) {
    memset(buf, 0, count);
    return (int32_t)count;
}

static int32_t zero_write(const void* buf, uint32_t count) {
    (void)buf;
    return (int32_t)count;  /* Accept and discard */
}

/* =========================================================================
 * Built-in Device: /dev/random (pseudo-random bytes)
 * ========================================================================= */

static uint32_t rand_state = 12345;

static int32_t random_read(void* buf, uint32_t count) {
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        /* Simple LCG PRNG seeded from tick counter */
        rand_state = rand_state * 1103515245 + 12345 + pit_get_ticks();
        p[i] = (uint8_t)((rand_state >> 16) & 0xFF);
    }
    return (int32_t)count;
}

/* =========================================================================
 * Built-in Device: /dev/disk (raw sector I/O)
 * ioctl cmd 0 = set sector number, read/write = transfer 512B
 * ========================================================================= */

static uint32_t disk_sector = 0;

static int32_t disk_read(void* buf, uint32_t count) {
    if (!g_ata_disk.present) return -1;
    if (count < 512) return -1;
    if (ata_read_sector(disk_sector, buf) != 0) return -1;
    return 512;
}

static int32_t disk_write(const void* buf, uint32_t count) {
    if (!g_ata_disk.present) return -1;
    if (count < 512) return -1;
    if (ata_write_sector(disk_sector, buf) != 0) return -1;
    return 512;
}

static int32_t disk_ioctl(uint32_t cmd, uint32_t arg) {
    if (cmd == 0) { disk_sector = arg; return 0; } /* Set sector */
    if (cmd == 1) { return (int32_t)g_ata_disk.sectors; } /* Get total sectors */
    return -1;
}

/* =========================================================================
 * Built-in Device: /dev/uptime (reads uptime as string)
 * ========================================================================= */

static int32_t uptime_read(void* buf, uint32_t count) {
    uint32_t up = pit_get_uptime();
    char tmp[32];
    int len = 0;
    /* Build string manually */
    uint32_t v = up;
    char digits[12];
    int di = 0;
    if (v == 0) digits[di++] = '0';
    else { while (v > 0) { digits[di++] = '0' + (v % 10); v /= 10; } }
    for (int i = di - 1; i >= 0; i--) tmp[len++] = digits[i];
    tmp[len++] = '\n';
    tmp[len] = '\0';
    if (count > (uint32_t)len) count = (uint32_t)len;
    memcpy(buf, tmp, count);
    return (int32_t)count;
}

/* =========================================================================
 * Initialize All Devices
 * ========================================================================= */

void dev_init(void) {
    memset(g_devices, 0, sizeof(g_devices));
    g_dev_count = 0;

    dev_register("console", DEV_TYPE_CHAR,
        (dev_ops_t){ console_read, console_write, NULL });

    dev_register("serial", DEV_TYPE_CHAR,
        (dev_ops_t){ serial_dev_read, serial_dev_write, NULL });

    dev_register("kbd", DEV_TYPE_CHAR,
        (dev_ops_t){ kbd_read, NULL, NULL });

    dev_register("null", DEV_TYPE_CHAR,
        (dev_ops_t){ null_read, null_write, NULL });

    dev_register("zero", DEV_TYPE_CHAR,
        (dev_ops_t){ zero_read, zero_write, NULL });

    dev_register("random", DEV_TYPE_CHAR,
        (dev_ops_t){ random_read, NULL, NULL });

    if (g_ata_disk.present) {
        dev_register("disk", DEV_TYPE_BLOCK,
            (dev_ops_t){ disk_read, disk_write, disk_ioctl });
    }

    dev_register("uptime", DEV_TYPE_CHAR,
        (dev_ops_t){ uptime_read, NULL, NULL });
}

/* =========================================================================
 * Dump — List All Devices
 * ========================================================================= */

void dev_dump(void) {
    vga_puts_color("=== Devices (ajhz) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_printf("  %u device(s) registered:\n\n", g_dev_count);
    vga_puts_color("  Name              Type   Reads   Writes  BytesR  BytesW\n", VGA_DARK_GREY, VGA_BLACK);
    vga_puts_color("  ----              ----   -----   ------  ------  ------\n", VGA_DARK_GREY, VGA_BLACK);

    for (uint32_t i = 0; i < g_dev_count; i++) {
        device_t* d = &g_devices[i];
        if (!d->registered) continue;

        vga_puts("  /dev/");
        vga_puts_color(d->name, VGA_WHITE, VGA_BLACK);

        /* Pad name to 12 chars */
        uint32_t nlen = strlen(d->name);
        for (uint32_t j = nlen; j < 12; j++) vga_putchar(' ');

        vga_puts(d->type == DEV_TYPE_CHAR ? "char " : "block");
        vga_puts("  ");

        /* Capabilities */
        vga_puts_color(d->ops.read  ? "R" : "-", d->ops.read  ? VGA_LIGHT_GREEN : VGA_DARK_GREY, VGA_BLACK);
        vga_puts_color(d->ops.write ? "W" : "-", d->ops.write ? VGA_LIGHT_GREEN : VGA_DARK_GREY, VGA_BLACK);
        vga_puts_color(d->ops.ioctl ? "I" : "-", d->ops.ioctl ? VGA_LIGHT_GREEN : VGA_DARK_GREY, VGA_BLACK);
        vga_puts("  ");

        vga_printf("%5u  %5u   %5u  %5u\n",
                   d->total_reads, d->total_writes,
                   d->bytes_read, d->bytes_written);
    }
}
