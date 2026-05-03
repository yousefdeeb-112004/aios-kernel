/* =============================================================================
 * ATA/IDE PIO Mode Disk Driver
 *
 * Supports primary IDE channel (ports 0x1F0-0x1F7, IRQ 14).
 * PIO mode: CPU reads/writes sectors directly via port I/O.
 * Each sector = 512 bytes.
 * ============================================================================= */

#include <drivers/ata.h>
#include <kernel/ports.h>
#include <kernel/log.h>
#include <drivers/vga.h>
#include <lib/string.h>

ata_disk_info_t g_ata_disk;

/* Wait for BSY to clear */
static void ata_wait_busy(void) {
    while (inb(ATA_PRIMARY_STATUS) & ATA_STATUS_BSY);
}

/* Wait for DRQ (data request) */
static bool ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_PRIMARY_STATUS);
        if (s & ATA_STATUS_ERR) return false;
        if (s & ATA_STATUS_DRQ) return true;
    }
    return false;
}

/* Software reset */
static void ata_soft_reset(void) {
    outb(ATA_PRIMARY_CTRL, 0x04);  /* Set SRST */
    for (volatile int i = 0; i < 10000; i++);
    outb(ATA_PRIMARY_CTRL, 0x00);  /* Clear SRST */
    ata_wait_busy();
}

/* 400ns delay (read status port 4 times) */
static void ata_delay(void) {
    inb(ATA_PRIMARY_STATUS);
    inb(ATA_PRIMARY_STATUS);
    inb(ATA_PRIMARY_STATUS);
    inb(ATA_PRIMARY_STATUS);
}

/* Read 16-bit words from data port */
static void ata_read_words(uint16_t* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint16_t v;
        __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"((uint16_t)ATA_PRIMARY_DATA));
        buf[i] = v;
    }
}

/* Write 16-bit words to data port */
static void ata_write_words(const uint16_t* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        __asm__ volatile("outw %0, %1" : : "a"(buf[i]), "Nd"((uint16_t)ATA_PRIMARY_DATA));
    }
}

/* Copy ATA identify string (byte-swapped) */
static void ata_copy_string(char* dst, const uint16_t* src, int words) {
    for (int i = 0; i < words; i++) {
        dst[i * 2]     = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    dst[words * 2] = '\0';
    /* Trim trailing spaces */
    int len = words * 2;
    while (len > 0 && dst[len - 1] == ' ') dst[--len] = '\0';
}

void ata_init(void) {
    memset(&g_ata_disk, 0, sizeof(ata_disk_info_t));
    g_ata_disk.present = false;

    /* Select master drive */
    outb(ATA_PRIMARY_DRIVE, 0xA0);
    ata_delay();

    /* Send IDENTIFY command */
    outb(ATA_PRIMARY_COUNT, 0);
    outb(ATA_PRIMARY_LBA_LO, 0);
    outb(ATA_PRIMARY_LBA_MID, 0);
    outb(ATA_PRIMARY_LBA_HI, 0);
    outb(ATA_PRIMARY_CMD, ATA_CMD_IDENTIFY);
    ata_delay();

    /* Check if drive exists */
    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0) {
        LOG_INFO_MSG("ATA", "No drive on primary master");
        return;
    }

    /* Wait for BSY to clear */
    ata_wait_busy();

    /* Check for non-ATA (ATAPI etc.) */
    if (inb(ATA_PRIMARY_LBA_MID) != 0 || inb(ATA_PRIMARY_LBA_HI) != 0) {
        LOG_INFO_MSG("ATA", "Non-ATA device detected");
        return;
    }

    /* Wait for DRQ or ERR */
    if (!ata_wait_drq()) {
        LOG_WARN_MSG("ATA", "IDENTIFY failed");
        return;
    }

    /* Read 256 words (512 bytes) of identify data */
    uint16_t identify[256];
    ata_read_words(identify, 256);

    /* Parse identify data */
    g_ata_disk.present = true;
    g_ata_disk.is_ata = true;

    /* Model string: words 27-46 (20 words = 40 chars) */
    ata_copy_string(g_ata_disk.model, &identify[27], 20);

    /* Serial number: words 10-19 (10 words = 20 chars) */
    ata_copy_string(g_ata_disk.serial, &identify[10], 10);

    /* Total LBA28 sectors: word 60-61 */
    g_ata_disk.sectors = identify[60] | ((uint32_t)identify[61] << 16);
    g_ata_disk.size_mb = g_ata_disk.sectors / 2048;

    LOG_INFO_MSG("ATA", "Disk detected on primary master");
}

int32_t ata_read_sector(uint32_t lba, void* buffer) {
    if (!g_ata_disk.present) return -1;

    ata_wait_busy();

    /* Select drive + LBA mode + top 4 bits of LBA */
    outb(ATA_PRIMARY_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_COUNT, 1);        /* 1 sector */
    outb(ATA_PRIMARY_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_CMD, ATA_CMD_READ_PIO);

    ata_delay();

    if (!ata_wait_drq()) {
        g_ata_disk.total_errors++;
        return -1;
    }

    ata_read_words((uint16_t*)buffer, 256);
    g_ata_disk.total_reads++;
    return 0;
}

int32_t ata_write_sector(uint32_t lba, const void* buffer) {
    if (!g_ata_disk.present) return -1;

    ata_wait_busy();

    outb(ATA_PRIMARY_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_COUNT, 1);
    outb(ATA_PRIMARY_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_CMD, ATA_CMD_WRITE_PIO);

    ata_delay();

    if (!ata_wait_drq()) {
        g_ata_disk.total_errors++;
        return -1;
    }

    ata_write_words((const uint16_t*)buffer, 256);

    /* Flush cache */
    outb(ATA_PRIMARY_CMD, 0xE7);
    ata_wait_busy();

    g_ata_disk.total_writes++;
    return 0;
}

void ata_dump(void) {
    vga_puts_color("=== Disk Info (qrs) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    if (!g_ata_disk.present) {
        vga_puts_color("  No disk detected.\n", VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("  Attach with: -hda disk.img\n");
        return;
    }
    vga_puts("  Model:   ");
    vga_puts_color(g_ata_disk.model, VGA_WHITE, VGA_BLACK);
    vga_puts("\n");
    vga_puts("  Serial:  ");
    vga_puts(g_ata_disk.serial);
    vga_puts("\n");
    vga_puts("  Sectors: ");
    vga_put_dec(g_ata_disk.sectors);
    vga_puts("\n");
    vga_puts("  Size:    ");
    vga_put_dec(g_ata_disk.size_mb);
    vga_puts(" MB\n");
    vga_puts("  Reads:   ");
    vga_put_dec(g_ata_disk.total_reads);
    vga_puts("  Writes: ");
    vga_put_dec(g_ata_disk.total_writes);
    vga_puts("  Errors: ");
    vga_put_dec(g_ata_disk.total_errors);
    vga_puts("\n");
}
