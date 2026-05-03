/* =============================================================================
 * AIOS-FS — Persistent Filesystem Implementation
 *
 * Bridges VFS (RAM) <-> ATA disk.
 * On format: writes superblock + empty file table.
 * On sync: writes all non-readonly VFS files to disk sequentially.
 * On load: reads file table from disk, recreates files in VFS.
 * ============================================================================= */

#include <kernel/aios_fs.h>
#include <kernel/vfs.h>
#include <drivers/ata.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <lib/string.h>
#include <lib/kprintf.h>

/* Sector buffer for disk I/O */
static uint8_t sector_buf[AIOSFS_SECTOR_SIZE];

/* === Format === */

int32_t aiosfs_format(void) {
    if (!g_ata_disk.present) {
        vga_printf("  AIOSFS: No disk attached!\n");
        return -1;
    }

    /* Write superblock */
    aiosfs_super_t super;
    memset(&super, 0, sizeof(super));
    super.magic = AIOSFS_MAGIC;
    super.version = AIOSFS_VERSION;
    super.file_count = 0;
    super.next_free_sector = AIOSFS_DATA_START;
    super.total_sectors = g_ata_disk.sectors;
    super.total_data_bytes = 0;
    super.format_tick = pit_get_ticks();
    super.last_sync_tick = 0;

    memset(sector_buf, 0, AIOSFS_SECTOR_SIZE);
    memcpy(sector_buf, &super, sizeof(super));
    if (ata_write_sector(AIOSFS_SUPERBLOCK, sector_buf) != 0) {
        vga_printf("  AIOSFS: Failed to write superblock!\n");
        return -1;
    }

    /* Clear file table sectors */
    memset(sector_buf, 0, AIOSFS_SECTOR_SIZE);
    for (uint32_t i = 0; i < AIOSFS_FTABLE_SECTORS; i++) {
        if (ata_write_sector(AIOSFS_FTABLE_START + i, sector_buf) != 0) {
            vga_printf("  AIOSFS: Failed to clear file table!\n");
            return -1;
        }
    }

    vga_puts_color("  AIOSFS: Disk formatted successfully.\n", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_printf("  Total: %u sectors (%u KB)\n",
               g_ata_disk.sectors, g_ata_disk.sectors / 2);
    return 0;
}

/* === Detect === */

bool aiosfs_detect(void) {
    if (!g_ata_disk.present) return false;
    if (ata_read_sector(AIOSFS_SUPERBLOCK, sector_buf) != 0) return false;
    aiosfs_super_t* super = (aiosfs_super_t*)sector_buf;
    return (super->magic == AIOSFS_MAGIC && super->version == AIOSFS_VERSION);
}

/* === Sync VFS -> Disk === */

int32_t aiosfs_sync(void) {
    if (!g_ata_disk.present) {
        vga_printf("  AIOSFS: No disk!\n");
        return -1;
    }

    /* Read current superblock */
    if (ata_read_sector(AIOSFS_SUPERBLOCK, sector_buf) != 0) return -1;
    aiosfs_super_t* super = (aiosfs_super_t*)sector_buf;
    if (super->magic != AIOSFS_MAGIC) {
        vga_printf("  AIOSFS: Disk not formatted. Use 'tnsyq' first.\n");
        return -1;
    }

    /* Collect non-readonly files from VFS */
    aiosfs_entry_t entries[AIOSFS_MAX_FILES];
    memset(entries, 0, sizeof(entries));
    uint32_t file_count = 0;
    uint32_t data_sector = AIOSFS_DATA_START;

    /* Iterate through VFS files */
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        const vfs_file_t* f = vfs_info_by_index(i);
        if (!f || !f->used) continue;
        if (f->readonly) continue; /* Skip system files like readme.txt */
        if (file_count >= AIOSFS_MAX_FILES) break;

        aiosfs_entry_t* e = &entries[file_count];
        memset(e->name, 0, AIOSFS_NAME_LEN);
        /* Safe copy of filename */
        uint32_t nlen = strlen(f->name);
        if (nlen > AIOSFS_NAME_LEN - 1) nlen = AIOSFS_NAME_LEN - 1;
        memcpy(e->name, f->name, nlen);
        e->start_sector = data_sector;
        e->size = f->size;
        e->flags = 0;

        /* Write file data to disk */
        uint32_t sectors_needed = (f->size + AIOSFS_SECTOR_SIZE - 1) / AIOSFS_SECTOR_SIZE;
        if (sectors_needed == 0) sectors_needed = 1; /* At least 1 sector */

        uint32_t bytes_written = 0;
        for (uint32_t s = 0; s < sectors_needed; s++) {
            memset(sector_buf, 0, AIOSFS_SECTOR_SIZE);
            uint32_t chunk = f->size - bytes_written;
            if (chunk > AIOSFS_SECTOR_SIZE) chunk = AIOSFS_SECTOR_SIZE;
            if (chunk > 0 && f->data) {
                memcpy(sector_buf, f->data + bytes_written, chunk);
            }
            if (ata_write_sector(data_sector + s, sector_buf) != 0) {
                vga_printf("  AIOSFS: Write error at sector %u!\n", data_sector + s);
                return -1;
            }
            bytes_written += chunk;
        }

        data_sector += sectors_needed;
        file_count++;
    }

    /* Write file table entries to disk */
    /* 8 entries per sector (64 bytes each, 512/64 = 8) */
    for (uint32_t s = 0; s < AIOSFS_FTABLE_SECTORS; s++) {
        memset(sector_buf, 0, AIOSFS_SECTOR_SIZE);
        for (uint32_t j = 0; j < 8; j++) {
            uint32_t idx = s * 8 + j;
            if (idx < file_count) {
                memcpy(sector_buf + j * AIOSFS_ENTRY_SIZE,
                       &entries[idx], AIOSFS_ENTRY_SIZE);
            }
        }
        if (ata_write_sector(AIOSFS_FTABLE_START + s, sector_buf) != 0) {
            vga_printf("  AIOSFS: Failed to write file table!\n");
            return -1;
        }
    }

    /* Update superblock */
    memset(sector_buf, 0, AIOSFS_SECTOR_SIZE);
    aiosfs_super_t new_super;
    memset(&new_super, 0, sizeof(new_super));
    new_super.magic = AIOSFS_MAGIC;
    new_super.version = AIOSFS_VERSION;
    new_super.file_count = file_count;
    new_super.next_free_sector = data_sector;
    new_super.total_sectors = g_ata_disk.sectors;
    new_super.total_data_bytes = 0;
    for (uint32_t i = 0; i < file_count; i++)
        new_super.total_data_bytes += entries[i].size;
    new_super.last_sync_tick = pit_get_ticks();
    memcpy(sector_buf, &new_super, sizeof(new_super));
    if (ata_write_sector(AIOSFS_SUPERBLOCK, sector_buf) != 0) return -1;

    vga_puts_color("  AIOSFS: Synced ", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_printf("%u files to disk (%u bytes)\n",
               file_count, new_super.total_data_bytes);
    return 0;
}

/* === Load Disk -> VFS === */

int32_t aiosfs_load(void) {
    if (!g_ata_disk.present) {
        vga_printf("  AIOSFS: No disk!\n");
        return -1;
    }

    /* Read superblock */
    if (ata_read_sector(AIOSFS_SUPERBLOCK, sector_buf) != 0) return -1;
    aiosfs_super_t* super = (aiosfs_super_t*)sector_buf;
    if (super->magic != AIOSFS_MAGIC) {
        vga_printf("  AIOSFS: No filesystem found. Use 'tnsyq' to format.\n");
        return -1;
    }

    uint32_t file_count = super->file_count;
    if (file_count == 0) {
        vga_printf("  AIOSFS: Disk is empty (0 files).\n");
        return 0;
    }
    if (file_count > AIOSFS_MAX_FILES) file_count = AIOSFS_MAX_FILES;

    /* Read file table entries */
    aiosfs_entry_t entries[AIOSFS_MAX_FILES];
    memset(entries, 0, sizeof(entries));

    for (uint32_t s = 0; s < AIOSFS_FTABLE_SECTORS; s++) {
        if (ata_read_sector(AIOSFS_FTABLE_START + s, sector_buf) != 0) return -1;
        for (uint32_t j = 0; j < 8; j++) {
            uint32_t idx = s * 8 + j;
            if (idx >= file_count) break;
            memcpy(&entries[idx], sector_buf + j * AIOSFS_ENTRY_SIZE,
                   AIOSFS_ENTRY_SIZE);
        }
    }

    /* Load each file into VFS */
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < file_count; i++) {
        aiosfs_entry_t* e = &entries[i];
        if (e->name[0] == '\0') continue;
        if (e->size == 0) continue;

        /* Skip if file already exists in VFS (system files) */
        if (vfs_exists(e->name)) continue;

        /* Read file data from disk */
        uint32_t sectors_needed = (e->size + AIOSFS_SECTOR_SIZE - 1) / AIOSFS_SECTOR_SIZE;
        /* Allocate temp buffer for reading */
        uint8_t read_buf[AIOSFS_SECTOR_SIZE];
        uint32_t total_size = e->size;

        /* Create file in VFS first with empty content */
        int32_t fidx = vfs_create(e->name, NULL, 0);
        if (fidx < 0) continue;

        /* Open for writing and write sector by sector */
        int32_t fd = vfs_open_mode(e->name, VFS_MODE_WRITE);
        if (fd < 0) continue;

        uint32_t bytes_read = 0;
        for (uint32_t s = 0; s < sectors_needed; s++) {
            if (ata_read_sector(e->start_sector + s, read_buf) != 0) break;
            uint32_t chunk = total_size - bytes_read;
            if (chunk > AIOSFS_SECTOR_SIZE) chunk = AIOSFS_SECTOR_SIZE;
            vfs_write(fd, read_buf, chunk);
            bytes_read += chunk;
        }
        vfs_close(fd);
        loaded++;
    }

    vga_puts_color("  AIOSFS: Loaded ", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_printf("%u files from disk\n", loaded);
    return 0;
}

/* === Dump info === */

void aiosfs_dump(void) {
    vga_puts_color("=== AIOS-FS Info ===\n", VGA_LIGHT_CYAN, VGA_BLACK);

    if (!g_ata_disk.present) {
        vga_printf("  No disk attached.\n");
        return;
    }

    if (ata_read_sector(AIOSFS_SUPERBLOCK, sector_buf) != 0) {
        vga_printf("  Failed to read superblock.\n");
        return;
    }

    aiosfs_super_t* super = (aiosfs_super_t*)sector_buf;
    if (super->magic != AIOSFS_MAGIC) {
        vga_printf("  Disk not formatted with AIOS-FS.\n");
        vga_printf("  Use 'tnsyq' to format.\n");
        return;
    }

    vga_printf("  Magic:    0x%X (AIOS)\n", super->magic);
    vga_printf("  Version:  %u\n", super->version);
    vga_printf("  Files:    %u\n", super->file_count);
    vga_printf("  Data:     %u bytes\n", super->total_data_bytes);
    vga_printf("  Disk:     %u sectors (%u KB)\n",
               super->total_sectors, super->total_sectors / 2);
    vga_printf("  Used:     sectors %u-%u (data)\n",
               AIOSFS_DATA_START, super->next_free_sector - 1);
    vga_printf("  Free:     %u sectors\n",
               super->total_sectors - super->next_free_sector);

    uint32_t sync_sec = super->last_sync_tick / 100;
    if (sync_sec > 0)
        vga_printf("  Last sync: %um%us after boot\n", sync_sec / 60, sync_sec % 60);
    else
        vga_printf("  Last sync: never\n");

    /* Show file list from disk */
    if (super->file_count > 0) {
        vga_puts_color("  On-disk files:\n", VGA_WHITE, VGA_BLACK);
        uint32_t count = super->file_count;
        if (count > AIOSFS_MAX_FILES) count = AIOSFS_MAX_FILES;

        for (uint32_t s = 0; s < AIOSFS_FTABLE_SECTORS; s++) {
            if (ata_read_sector(AIOSFS_FTABLE_START + s, sector_buf) != 0) break;
            for (uint32_t j = 0; j < 8; j++) {
                uint32_t idx = s * 8 + j;
                if (idx >= count) break;
                aiosfs_entry_t* e = (aiosfs_entry_t*)(sector_buf + j * AIOSFS_ENTRY_SIZE);
                if (e->name[0] == '\0') continue;
                vga_printf("    %-20s %5u bytes (sec %u)\n",
                           e->name, e->size, e->start_sector);
            }
        }
    }
}
