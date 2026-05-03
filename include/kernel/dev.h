/* =============================================================================
 * Device Abstraction Layer — /dev
 *
 * Provides a unified interface for all hardware devices.
 * Each device registers read/write/ioctl operations.
 * User programs and shell commands interact with devices through handles.
 *
 * Built-in devices:
 *   /dev/console  — VGA text output + keyboard input
 *   /dev/serial   — COM1 serial port (read/write)
 *   /dev/kbd      — Raw keyboard input (read-only)
 *   /dev/null     — Discards writes, reads return 0
 *   /dev/zero     — Reads return zero bytes, writes discarded
 *   /dev/random   — Pseudo-random bytes (read-only)
 *   /dev/disk     — Raw ATA disk access (sector I/O)
 *
 * Shell commands (Arabic):
 *   ajhz  (أجهزة ajhiza = devices)  — list all registered devices
 *   dev r <name> [n]  — read n bytes from device
 *   dev w <name> <text> — write text to device
 * ============================================================================= */
#ifndef _KERNEL_DEV_H
#define _KERNEL_DEV_H

#include <kernel/types.h>

#define DEV_MAX        16
#define DEV_NAME_MAX   24

/* Device types */
typedef enum {
    DEV_TYPE_CHAR,      /* Character device (byte stream) */
    DEV_TYPE_BLOCK,     /* Block device (sector I/O) */
} dev_type_t;

/* Device operations — each device implements some or all */
typedef struct dev_ops {
    int32_t (*read)(void* buf, uint32_t count);
    int32_t (*write)(const void* buf, uint32_t count);
    int32_t (*ioctl)(uint32_t cmd, uint32_t arg);
} dev_ops_t;

/* Device descriptor */
typedef struct {
    char        name[DEV_NAME_MAX];   /* e.g. "console", "serial", "null" */
    dev_type_t  type;
    dev_ops_t   ops;
    bool        registered;
    /* Stats */
    uint32_t    total_reads;
    uint32_t    total_writes;
    uint32_t    bytes_read;
    uint32_t    bytes_written;
} device_t;

/* Global device table */
extern device_t g_devices[DEV_MAX];
extern uint32_t g_dev_count;

/* Register a device. Returns device index or -1 on failure. */
int32_t dev_register(const char* name, dev_type_t type, dev_ops_t ops);

/* Find device by name. Returns pointer or NULL. */
device_t* dev_find(const char* name);

/* Read from a named device */
int32_t dev_read(const char* name, void* buf, uint32_t count);

/* Write to a named device */
int32_t dev_write(const char* name, const void* buf, uint32_t count);

/* Ioctl on a named device */
int32_t dev_ioctl(const char* name, uint32_t cmd, uint32_t arg);

/* Initialize all built-in devices */
void dev_init(void);

/* List all devices (shell command) */
void dev_dump(void);

#endif
