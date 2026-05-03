/* =============================================================================
 * Virtual File System — Professional Edition
 *
 * Supports: create, open, read, write, append, seek, delete, rename,
 * truncate, file info, and metadata (created_tick, modified_tick, size).
 *
 * Max 4KB per file. Max 32 files. File names up to 63 chars.
 * ============================================================================= */
#ifndef _KERNEL_VFS_H
#define _KERNEL_VFS_H

#include <kernel/types.h>

#define VFS_MAX_FILES   64
#define VFS_MAX_NAME    64
#define VFS_MAX_OPEN    32
#define VFS_FILE_MAXSZ  65536  /* 64KB max per file */

/* Open modes */
#define VFS_MODE_READ   0x01
#define VFS_MODE_WRITE  0x02
#define VFS_MODE_APPEND 0x04
#define VFS_MODE_CREATE 0x08

/* File entry */
typedef struct {
    char     name[VFS_MAX_NAME];
    uint8_t* data;
    uint32_t size;          /* Current data size */
    uint32_t capacity;      /* Allocated buffer size */
    bool     used;
    bool     readonly;
    uint32_t created_tick;  /* Tick when created */
    uint32_t modified_tick; /* Tick when last written */
    uint32_t created_time;  /* RTC unix timestamp when created */
    uint32_t modified_time; /* RTC unix timestamp when last modified */
    bool     is_directory;  /* true = directory, false = regular file */
    char     parent[VFS_MAX_NAME]; /* Parent directory path */
    uint32_t open_count;
    uint32_t read_count;
    uint32_t write_count;
    uint32_t total_bytes_read;
    uint32_t total_bytes_written;
} vfs_file_t;

/* File descriptor */
typedef struct {
    uint32_t file_idx;
    uint32_t offset;
    uint8_t  mode;
    bool     used;
} vfs_fd_t;

/* Stats */
typedef struct {
    uint32_t total_files;
    uint32_t total_size;
    uint32_t open_fds;
    uint32_t total_opens;
    uint32_t total_reads;
    uint32_t total_writes;
    uint32_t total_bytes_read;
    uint32_t total_bytes_written;
    uint32_t total_deletes;
} vfs_stats_t;

extern vfs_stats_t g_vfs_stats;

/* Init + create default files */
void vfs_init(void);

/* Create a file with initial data (NULL data = empty file) */
int32_t vfs_create(const char* name, const void* data, uint32_t size);

/* Create an empty file */
int32_t vfs_create_empty(const char* name);

/* Open a file. mode = VFS_MODE_READ | VFS_MODE_WRITE etc. */
int32_t vfs_open(const char* name);
int32_t vfs_open_mode(const char* name, uint8_t mode);

/* Read from file descriptor */
int32_t vfs_read(int32_t fd, void* buf, uint32_t count);

/* Write to file descriptor (at current offset or append) */
int32_t vfs_write(int32_t fd, const void* buf, uint32_t count);

/* Seek to position */
int32_t vfs_seek(int32_t fd, uint32_t offset);

/* Close file descriptor */
void vfs_close(int32_t fd);

/* Delete a file by name */
int32_t vfs_delete(const char* name);

/* Rename a file */
int32_t vfs_rename(const char* old_name, const char* new_name);

/* Truncate file to zero length */
int32_t vfs_truncate(const char* name);

/* Check if file exists */
bool vfs_exists(const char* name);

/* Get file size (returns -1 if not found) */
int32_t vfs_filesize(const char* name);

/* Get file info (returns pointer to file entry, NULL if not found) */
const vfs_file_t* vfs_info(const char* name);

/* Get file info by table index (for iterating all files) */
const vfs_file_t* vfs_info_by_index(int index);

/* List files (to VGA) */
void vfs_list(void);

/* Dump VFS stats */
void vfs_dump(void);

/* === Directory support === */
int32_t vfs_mkdir(const char* name);
int32_t vfs_chdir(const char* name);
const char* vfs_getcwd(void);
int32_t vfs_rmdir(const char* name);

/* Resolve path relative to current directory */
void vfs_resolve_path(const char* input, char* output, uint32_t len);

#endif
