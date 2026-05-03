/* =============================================================================
 * ATA/IDE PIO Mode Disk Driver
 *
 * Supports primary IDE channel (ports 0x1F0-0x1F7, IRQ 14).
 * PIO mode: CPU reads/writes sectors directly via port I/O.
 * Each sector = 512 bytes.
 *
 * Shell commands (Arabic):
 *   qrs  (قرص qurs = disk)    — show disk info
 *   qra  (قراءة qira'a = read)  — read sector: qra <num>
 *   hfz  (حفظ hifz = save)    — write sector: hfz <num> <text>
 *
 * QEMU: run with -hda disk.img to attach a virtual hard disk
 * ============================================================================= */
#ifndef _DRIVERS_ATA_H
#define _DRIVERS_ATA_H

#include <kernel/types.h>

/* ATA I/O Ports (Primary channel) */
#define ATA_PRIMARY_DATA    0x1F0
#define ATA_PRIMARY_ERROR   0x1F1
#define ATA_PRIMARY_COUNT   0x1F2
#define ATA_PRIMARY_LBA_LO  0x1F3
#define ATA_PRIMARY_LBA_MID 0x1F4
#define ATA_PRIMARY_LBA_HI  0x1F5
#define ATA_PRIMARY_DRIVE   0x1F6
#define ATA_PRIMARY_CMD     0x1F7
#define ATA_PRIMARY_STATUS  0x1F7
#define ATA_PRIMARY_CTRL    0x3F6

/* ATA Commands */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_IDENTIFY    0xEC

/* ATA Status bits */
#define ATA_STATUS_BSY      0x80    /* Busy */
#define ATA_STATUS_DRDY     0x40    /* Drive ready */
#define ATA_STATUS_DRQ      0x08    /* Data request */
#define ATA_STATUS_ERR      0x01    /* Error */

#define ATA_SECTOR_SIZE     512

/* Disk info from IDENTIFY command */
typedef struct {
    bool     present;           /* Disk detected? */
    bool     is_ata;            /* ATA (not ATAPI)? */
    char     model[41];         /* Model string */
    char     serial[21];        /* Serial number */
    uint32_t sectors;           /* Total sectors (LBA28) */
    uint32_t size_mb;           /* Size in MB */
    /* AI metrics */
    uint32_t total_reads;
    uint32_t total_writes;
    uint32_t total_errors;
} ata_disk_info_t;

extern ata_disk_info_t g_ata_disk;

/* Initialize ATA driver + detect disk */
void ata_init(void);

/* Read one sector (512 bytes) from LBA address into buffer.
 * Returns 0 on success, -1 on error. */
int32_t ata_read_sector(uint32_t lba, void* buffer);

/* Write one sector (512 bytes) from buffer to LBA address.
 * Returns 0 on success, -1 on error. */
int32_t ata_write_sector(uint32_t lba, const void* buffer);

/* Print disk info */
void ata_dump(void);

#endif
