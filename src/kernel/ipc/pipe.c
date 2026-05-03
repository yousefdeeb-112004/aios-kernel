/* =============================================================================
 * Pipe / IPC — Implementation
 *
 * Ring buffer design:
 *   write_pos advances when producer writes.
 *   read_pos advances when consumer reads.
 *   count tracks bytes in buffer.
 *   Buffer wraps around using modulo PIPE_BUF_SIZE.
 * ============================================================================= */

#include <kernel/pipe.h>
#include <kernel/lock.h>
#include <kernel/process.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <lib/string.h>
#include <lib/kprintf.h>

pipe_t g_pipes[PIPE_MAX];
uint32_t g_pipe_count = 0;
static klock_t pipe_lock = KLOCK_INIT;

void pipe_init(void) {
    memset(g_pipes, 0, sizeof(g_pipes));
    g_pipe_count = 0;
}

int32_t pipe_create(const char* name) {
    klock_acquire(&pipe_lock);
    /* Check if name already exists */
    for (uint32_t i = 0; i < PIPE_MAX; i++) {
        if (g_pipes[i].active && strcmp(g_pipes[i].name, name) == 0) {
            klock_release(&pipe_lock);
            return -2; /* Already exists */
        }
    }
    /* Find free slot */
    for (uint32_t i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].active) {
            memset(&g_pipes[i], 0, sizeof(pipe_t));
            uint32_t nlen = strlen(name);
            if (nlen > PIPE_NAME_MAX - 1) nlen = PIPE_NAME_MAX - 1;
            memcpy(g_pipes[i].name, name, nlen);
            g_pipes[i].name[nlen] = '\0';
            g_pipes[i].active = true;
            g_pipe_count++;
            klock_release(&pipe_lock);
            return (int32_t)i;
        }
    }
    klock_release(&pipe_lock);
    return -1; /* No free slots */
}

int32_t pipe_destroy(const char* name) {
    klock_acquire(&pipe_lock);
    for (uint32_t i = 0; i < PIPE_MAX; i++) {
        if (g_pipes[i].active && strcmp(g_pipes[i].name, name) == 0) {
            g_pipes[i].active = false;
            g_pipe_count--;
            klock_release(&pipe_lock);
            return 0;
        }
    }
    klock_release(&pipe_lock);
    return -1;
}

pipe_t* pipe_find(const char* name) {
    for (uint32_t i = 0; i < PIPE_MAX; i++) {
        if (g_pipes[i].active && strcmp(g_pipes[i].name, name) == 0)
            return &g_pipes[i];
    }
    return NULL;
}

int32_t pipe_write(pipe_t* p, const void* buf, uint32_t count) {
    if (!p || !p->active) return -1;
    const uint8_t* src = (const uint8_t*)buf;
    uint32_t written = 0;

    klock_acquire(&pipe_lock);
    for (uint32_t i = 0; i < count; i++) {
        if (p->count >= PIPE_BUF_SIZE) {
            p->write_waits++;
            break; /* Buffer full */
        }
        p->buffer[p->write_pos] = src[i];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        p->count++;
        written++;
    }
    p->total_written += written;
    klock_release(&pipe_lock);
    return (int32_t)written;
}

int32_t pipe_read(pipe_t* p, void* buf, uint32_t count) {
    if (!p || !p->active) return -1;
    uint8_t* dst = (uint8_t*)buf;
    uint32_t rd = 0;

    klock_acquire(&pipe_lock);
    for (uint32_t i = 0; i < count; i++) {
        if (p->count == 0) {
            p->read_waits++;
            break; /* Buffer empty */
        }
        dst[i] = p->buffer[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->count--;
        rd++;
    }
    p->total_read += rd;
    klock_release(&pipe_lock);
    return (int32_t)rd;
}

int32_t pipe_read_blocking(pipe_t* p, void* buf, uint32_t count, uint32_t timeout_ms) {
    if (!p || !p->active) return -1;
    uint32_t start = pit_get_ticks();
    uint32_t timeout_ticks = timeout_ms / 10; /* PIT at 100Hz */
    if (timeout_ms > 0 && timeout_ticks == 0) timeout_ticks = 1;

    while (p->count == 0 && p->active) {
        if (timeout_ms > 0 && (pit_get_ticks() - start) >= timeout_ticks)
            return 0; /* Timeout */
        proc_yield();
    }
    return pipe_read(p, buf, count);
}

uint32_t pipe_available(pipe_t* p) {
    return p ? p->count : 0;
}

uint32_t pipe_free_space(pipe_t* p) {
    return p ? (PIPE_BUF_SIZE - p->count) : 0;
}

void pipe_dump(void) {
    vga_puts_color("=== Pipes (anbb) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    if (g_pipe_count == 0) {
        vga_puts("  No active pipes.\n");
        vga_puts("  Create: anbb mk <name>\n");
        vga_puts("  Demo:   anbb demo\n");
        return;
    }
    vga_printf("  %u active pipe(s):\n\n", g_pipe_count);
    vga_puts_color("  Name              Buffered  Written   Read     Waits(W/R)\n",
                   VGA_DARK_GREY, VGA_BLACK);
    for (uint32_t i = 0; i < PIPE_MAX; i++) {
        pipe_t* p = &g_pipes[i];
        if (!p->active) continue;
        vga_puts("  |");
        vga_puts_color(p->name, VGA_WHITE, VGA_BLACK);
        uint32_t nlen = strlen(p->name);
        for (uint32_t j = nlen; j < 16; j++) vga_putchar(' ');
        vga_printf("%4u/%u   %6u   %6u   %u/%u\n",
                   p->count, PIPE_BUF_SIZE,
                   p->total_written, p->total_read,
                   p->write_waits, p->read_waits);
    }
}

/* =========================================================================
 * Demo: Producer/Consumer processes communicating via pipe
 * ========================================================================= */

static pipe_t* demo_pipe = NULL;

static void producer_fn(void) {
    const char* messages[] = {
        "Hello from producer!\n",
        "Message 2: AIOS IPC works!\n",
        "Message 3: Pipe buffer is 1KB\n",
        "Message 4: Processes communicate!\n",
        "DONE\n"
    };
    for (int i = 0; i < 5; i++) {
        pit_sleep_ms(300);
        if (!demo_pipe || !demo_pipe->active) break;
        const char* msg = messages[i];
        pipe_write(demo_pipe, msg, strlen(msg));
        vga_puts_color("  [P] sent: ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts(msg);
    }
}

static void consumer_fn(void) {
    char buf[128];
    for (int i = 0; i < 10; i++) {
        if (!demo_pipe || !demo_pipe->active) break;
        int32_t n = pipe_read_blocking(demo_pipe, buf, 127, 2000);
        if (n > 0) {
            buf[n] = '\0';
            vga_puts_color("  [C] recv: ", VGA_LIGHT_CYAN, VGA_BLACK);
            vga_puts(buf);
            /* Check for termination */
            if (n >= 4 && buf[0] == 'D' && buf[1] == 'O' &&
                buf[2] == 'N' && buf[3] == 'E') break;
        } else {
            vga_puts_color("  [C] timeout\n", VGA_YELLOW, VGA_BLACK);
        }
    }
}

void pipe_demo(void) {
    vga_puts_color("=== Pipe Demo (anbb demo) ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Creating pipe 'demo'...\n");

    int32_t idx = pipe_create("demo");
    if (idx < 0) {
        /* Pipe might already exist, find it */
        demo_pipe = pipe_find("demo");
        if (!demo_pipe) {
            vga_puts_color("  Failed to create pipe!\n", VGA_LIGHT_RED, VGA_BLACK);
            return;
        }
    } else {
        demo_pipe = &g_pipes[idx];
    }

    vga_puts("  Launching producer (P) and consumer (C) processes...\n\n");

    proc_create("producer", producer_fn, 128);
    proc_create("consumer", consumer_fn, 128);

    /* Wait for demo to complete */
    pit_sleep_ms(3000);

    vga_puts("\n  Demo complete. ");
    vga_printf("Pipe stats: written=%u read=%u\n",
               demo_pipe->total_written, demo_pipe->total_read);

    pipe_destroy("demo");
    demo_pipe = NULL;
}
