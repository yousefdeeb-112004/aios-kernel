/* =============================================================================
 * AIOS-FS — Simple Persistent Filesystem
 *
 * On-disk layout:
 *   Sector 0:   Superblock (magic, version, file count, first free sector)
 *   Sector 1-8: File table (up to 32 entries, 4 sectors)
 *   Sector 9+:  File data blocks
 *
 * Each file entry = 64 bytes:
 *   name[48], start_sector(u32), size(u32), flags(u32), reserved(u32)
 *
 * Simple contiguous allocation: each file gets consecutive sectors.
 * Max disk size with this format: 1MB (2048 sectors).
 *
 * Shell commands (Arabic):
 *   hfzk  (حفظ الكل = save all)   — sync VFS to disk
 *   hmml  (تحميل = load)         — load VFS from disk
 *   tnsyq (تنسيق = format)       — format disk with AIOS-FS
 *   auto-sync on: ktb, thrr save, hdhf
 * ============================================================================= */
#ifndef _KERNEL_AIOS_FS_H
#define _KERNEL_AIOS_FS_H

#include <kernel/types.h>

#define AIOSFS_MAGIC       0x41494F53   /* "AIOS" */
#define AIOSFS_VERSION     1
#define AIOSFS_MAX_FILES   64
#define AIOSFS_SUPERBLOCK  0            /* Sector 0 */
#define AIOSFS_FTABLE_START 1           /* Sectors 1-4 */
#define AIOSFS_FTABLE_SECTORS 4
#define AIOSFS_DATA_START  5            /* First data sector */
#define AIOSFS_ENTRY_SIZE  64           /* Bytes per file entry */
#define AIOSFS_SECTOR_SIZE 512

/* Superblock (fits in first 64 bytes of sector 0) */
typedef struct {
    uint32_t magic;             /* AIOSFS_MAGIC */
    uint32_t version;           /* AIOSFS_VERSION */
    uint32_t file_count;        /* Number of files on disk */
    uint32_t next_free_sector;  /* Next free sector for data */
    uint32_t total_sectors;     /* Total sectors on disk */
    uint32_t total_data_bytes;  /* Total bytes of file data */
    uint32_t format_tick;       /* Tick when formatted */
    uint32_t last_sync_tick;    /* Tick when last synced */
    uint8_t  reserved[480];     /* Pad to 512 bytes */
} __attribute__((packed)) aiosfs_super_t;

/* File entry on disk (64 bytes) */
#define AIOSFS_NAME_LEN 48
#define AIOSFS_FLAG_READONLY 0x01
#define AIOSFS_FLAG_SYSTEM   0x02

typedef struct {
    char     name[AIOSFS_NAME_LEN]; /* Null-terminated filename */
    uint32_t start_sector;          /* First sector of data */
    uint32_t size;                  /* Size in bytes */
    uint32_t flags;                 /* AIOSFS_FLAG_* */
    uint32_t reserved;
} __attribute__((packed)) aiosfs_entry_t;

/* Format disk with AIOS-FS (erases everything!) */
int32_t aiosfs_format(void);

/* Check if disk has valid AIOS-FS */
bool aiosfs_detect(void);

/* Sync all VFS files to disk */
int32_t aiosfs_sync(void);

/* Load all files from disk into VFS */
int32_t aiosfs_load(void);

/* Dump AIOS-FS info */
void aiosfs_dump(void);

#endif
