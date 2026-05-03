/* =============================================================================
 * Pipe / IPC — Inter-Process Communication
 *
 * Provides unidirectional byte-stream pipes between kernel processes.
 * One process writes, another reads. Uses a ring buffer internally.
 *
 * Features:
 *   - Up to 8 pipes simultaneously
 *   - 1024-byte ring buffer per pipe
 *   - Blocking read (yields until data available)
 *   - Non-blocking write (returns -1 if full)
 *   - Named pipes for shell accessibility
 *
 * Shell commands (Arabic):
 *   anbb   (أنبوب unbub = pipe)  — list active pipes
 *   anbb mk <name>              — create a named pipe
 *   anbb rm <name>              — destroy a pipe
 *   anbb w <name> <text>        — write text to pipe
 *   anbb r <name>               — read from pipe
 *   anbb demo                   — producer/consumer demo
 * ============================================================================= */
#ifndef _KERNEL_PIPE_H
#define _KERNEL_PIPE_H

#include <kernel/types.h>

#define PIPE_MAX       8
#define PIPE_BUF_SIZE  1024
#define PIPE_NAME_MAX  24

typedef struct {
    char     name[PIPE_NAME_MAX];
    uint8_t  buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;     /* Consumer reads from here */
    uint32_t write_pos;    /* Producer writes here */
    uint32_t count;        /* Bytes in buffer */
    bool     active;
    /* Stats */
    uint32_t total_written;
    uint32_t total_read;
    uint32_t write_waits;  /* Times writer found buffer full */
    uint32_t read_waits;   /* Times reader found buffer empty */
} pipe_t;

/* Pipe table */
extern pipe_t g_pipes[PIPE_MAX];
extern uint32_t g_pipe_count;

/* Create a named pipe. Returns pipe index or -1. */
int32_t pipe_create(const char* name);

/* Destroy a pipe by name. Returns 0 or -1. */
int32_t pipe_destroy(const char* name);

/* Find pipe by name. Returns pointer or NULL. */
pipe_t* pipe_find(const char* name);

/* Write bytes to pipe. Returns bytes written or -1 if full. */
int32_t pipe_write(pipe_t* p, const void* buf, uint32_t count);

/* Read bytes from pipe. Returns bytes read (0 if empty). */
int32_t pipe_read(pipe_t* p, void* buf, uint32_t count);

/* Blocking read: yields until at least 1 byte is available.
 * timeout_ms: max wait time (0 = wait forever). Returns bytes read. */
int32_t pipe_read_blocking(pipe_t* p, void* buf, uint32_t count, uint32_t timeout_ms);

/* Check how many bytes available to read */
uint32_t pipe_available(pipe_t* p);

/* Check how many bytes of free space for writing */
uint32_t pipe_free_space(pipe_t* p);

/* Init pipe system */
void pipe_init(void);

/* List active pipes */
void pipe_dump(void);

/* Demo: producer/consumer processes communicating via pipe */
void pipe_demo(void);

#endif
