/* =============================================================================
 * Virtual File System — with Directories + RTC timestamps
 * ============================================================================= */

#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <drivers/vga.h>
#include <drivers/pit.h>
#include <drivers/rtc.h>
#include <lib/string.h>

extern const uint8_t user_hello_elf[];
extern const uint32_t user_hello_elf_len;
extern const uint8_t user_uctest_elf[];
extern const uint32_t user_uctest_elf_len;
extern const uint8_t user_nawa_elf[];
extern const uint32_t user_nawa_elf_len;

static vfs_file_t ft[VFS_MAX_FILES];
static vfs_fd_t fdt[VFS_MAX_OPEN];
vfs_stats_t g_vfs_stats;

/* Current working directory */
static char cwd[VFS_MAX_NAME] = "/";

static int find_file(const char* name) {
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (ft[i].used && strcmp(ft[i].name, name) == 0) return i;
    return -1;
}

/* Resolve path: prepend CWD if relative */
void vfs_resolve_path(const char* input, char* output, uint32_t len) {
    if (!input || !output || len < 2) return;
    if (input[0] == '/') {
        /* Absolute path */
        strncpy(output, input, len - 1);
        output[len - 1] = '\0';
    } else {
        /* Relative to CWD */
        if (strcmp(cwd, "/") == 0) {
            output[0] = '/';
            strncpy(output + 1, input, len - 2);
            output[len - 1] = '\0';
        } else {
            strncpy(output, cwd, len - 1);
            uint32_t cl = strlen(output);
            if (cl > 0 && output[cl - 1] != '/' && cl < len - 2) {
                output[cl++] = '/';
            }
            strncpy(output + cl, input, len - cl - 1);
            output[len - 1] = '\0';
        }
    }
}

/* Get parent directory of a path */
static void parent_dir(const char* path, char* out, uint32_t len) {
    strncpy(out, path, len - 1);
    out[len - 1] = '\0';
    uint32_t l = strlen(out);
    /* Remove trailing slash */
    if (l > 1 && out[l - 1] == '/') { out[l - 1] = '\0'; l--; }
    /* Find last slash */
    while (l > 0 && out[l - 1] != '/') l--;
    if (l <= 1) { out[0] = '/'; out[1] = '\0'; }
    else out[l] = '\0';
}

void vfs_init(void) {
    memset(ft, 0, sizeof(ft));
    memset(fdt, 0, sizeof(fdt));
    memset(&g_vfs_stats, 0, sizeof(vfs_stats_t));
    strcpy(cwd, "/");

    /* Create root directory */
    vfs_mkdir("/");

    vfs_create("readme.txt",
        "AIOS Kernel - AI Operating System\n"
        "=================================\n"
        "Version: 2.0\n"
        "Author:  AL_Yousef_Deeb\n"
        "\n"
        "Features:\n"
        "  - 9-phase kernel + Arabic shell with 55+ commands\n"
        "  - User Mode (Ring 3) + ELF Loader + signals + sbrk\n"
        "  - Persistent AIOS-FS with directories & RTC timestamps\n"
        "  - Per-process address spaces + SMP scheduling\n"
        "  - UDP/IP + DHCP + Echo server\n"
        "  - Window Manager + AI Agent + DevTracker\n"
        "\n"
        "Type 'sd' for command list.\n", 0);
    int ri = find_file("readme.txt");
    if (ri >= 0) ft[ri].readonly = true;

    vfs_create("hello.txt",
        "Marhaba! Hello from AIOS kernel!\n"
        "This file was created at boot time.\n", 0);

    vfs_create("config.cfg",
        "# AIOS Kernel Configuration\n"
        "ai_features=enabled\n"
        "log_level=info\n"
        "timer_hz=100\n"
        "shell=aios_shell\n"
        "elf_loader=enabled\n"
        "network=rtl8139\n"
        "smp=auto\n"
        "rtc=enabled\n"
        "directories=enabled\n", 0);

    vfs_create("notes.txt",
        "Welcome to AIOS!\n"
        "You can edit this file with: thrr notes.txt\n", 0);

    vfs_create("hello.elf", user_hello_elf, user_hello_elf_len);
    ri = find_file("hello.elf");
    if (ri >= 0) ft[ri].readonly = true;

    vfs_create("uctest.elf", user_uctest_elf, user_uctest_elf_len);
    ri = find_file("uctest.elf");
    if (ri >= 0) ft[ri].readonly = true;

    vfs_create("nawa.elf", user_nawa_elf, user_nawa_elf_len);
    ri = find_file("nawa.elf");
    if (ri >= 0) ft[ri].readonly = true;

    /* Bootstrap script for the nawa interpreter (shgl nawa.elf). Seeded as a
     * plain text file exactly like readme.txt/notes.txt above, so it can be
     * read and edited from the shell. */
    vfs_create("boot.nw",
        "\\ nawa bootstrap\n"
        ": square dup * ;\n"
        ": cube dup square * ;\n"
        "\\ control flow (v1): counted loop + conditional min/max\n"
        ": rng 6 1 do i . loop cr ;\n"
        ": max over over < if swap then drop ;\n"
        ": min over over > if swap then drop ;\n"
        "5 cube . cr\n"
        "rng\n"
        "3 9 max . 3 9 min . cr\n"
        "\\ memory (v1): a stateful counter + a constant\n"
        "variable ctr\n"
        ": bump ctr @ 1 + ctr ! ;\n"
        "0 ctr ! bump bump bump\n"
        "ctr @ . cr\n"
        "10 constant ten\n"
        "ten . cr\n"
        "\\ self-extension (v1): a NEW control word `unless` written in nawa,\n"
        "\\ then used as syntax inside `check` (prints only when flag is zero)\n"
        ": unless immediate postpone 0= postpone if ;\n"
        "variable flag\n"
        ": check flag @ unless 99 . cr then ;\n"
        "0 flag ! check\n"
        "7 flag ! check\n"
        "\\ cross-layer: a nawa program reasoning about LIVE kernel AI state.\n"
        "\\ ai-p1 pushes node 1's posterior permille; print it then a Y/N verdict\n"
        "\\ (89=Y,78=N) on whether it crosses 600 (0.1%% units).\n"
        ": leak? ai-p1 dup . 600 > if 89 else 78 then emit cr ;\n"
        "leak?\n", 0);
}

int32_t vfs_create(const char* name, const void* data, uint32_t size) {
    if (find_file(name) >= 0) return -2;

    int s = -1;
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (!ft[i].used) { s = i; break; }
    if (s < 0) return -1;

    if (data && size == 0) size = strlen((const char*)data);

    uint32_t cap = size;
    if (cap < 256) cap = 256;
    if (cap > VFS_FILE_MAXSZ) cap = VFS_FILE_MAXSZ;

    uint8_t* buf = (uint8_t*)kmalloc(cap);
    if (!buf) return -1;
    memset(buf, 0, cap);

    if (data && size > 0) {
        uint32_t copy_sz = (size > cap) ? cap : size;
        memcpy(buf, data, copy_sz);
        size = copy_sz;
    }

    vfs_file_t* f = &ft[s];
    memset(f, 0, sizeof(vfs_file_t));
    strcpy(f->name, name);
    f->data = buf;
    f->size = size;
    f->capacity = cap;
    f->used = true;
    f->is_directory = false;
    f->created_tick = pit_get_ticks();
    f->modified_tick = f->created_tick;
    f->created_time = rtc_unix_approx();
    f->modified_time = f->created_time;
    strcpy(f->parent, cwd);

    g_vfs_stats.total_files++;
    g_vfs_stats.total_size += size;
    return s;
}

int32_t vfs_create_empty(const char* name) {
    return vfs_create(name, NULL, 0);
}

/* === Directory operations === */

int32_t vfs_mkdir(const char* name) {
    if (find_file(name) >= 0) return -2; /* Already exists */

    int s = -1;
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (!ft[i].used) { s = i; break; }
    if (s < 0) return -1;

    vfs_file_t* f = &ft[s];
    memset(f, 0, sizeof(vfs_file_t));
    strcpy(f->name, name);
    f->data = NULL;
    f->size = 0;
    f->capacity = 0;
    f->used = true;
    f->is_directory = true;
    f->created_tick = pit_get_ticks();
    f->modified_tick = f->created_tick;
    f->created_time = rtc_unix_approx();
    f->modified_time = f->created_time;
    strcpy(f->parent, cwd);

    g_vfs_stats.total_files++;
    return s;
}

int32_t vfs_chdir(const char* name) {
    if (strcmp(name, "/") == 0) {
        strcpy(cwd, "/");
        return 0;
    }
    if (strcmp(name, "..") == 0) {
        parent_dir(cwd, cwd, VFS_MAX_NAME);
        return 0;
    }
    if (strcmp(name, ".") == 0) return 0;

    /* Try as-is first */
    int fi = find_file(name);
    if (fi >= 0 && ft[fi].is_directory) {
        strcpy(cwd, name);
        return 0;
    }

    /* Try relative to CWD */
    char full[VFS_MAX_NAME];
    vfs_resolve_path(name, full, VFS_MAX_NAME);
    fi = find_file(full);
    if (fi >= 0 && ft[fi].is_directory) {
        strcpy(cwd, full);
        return 0;
    }

    return -1; /* Not found or not a directory */
}

const char* vfs_getcwd(void) {
    return cwd;
}

int32_t vfs_rmdir(const char* name) {
    int fi = find_file(name);
    if (fi < 0) return -1;
    if (!ft[fi].is_directory) return -2;
    if (strcmp(name, "/") == 0) return -3; /* Can't remove root */

    /* Check if empty — no files with this as parent */
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!ft[i].used || i == fi) continue;
        if (strcmp(ft[i].parent, name) == 0) return -4; /* Not empty */
    }

    ft[fi].used = false;
    g_vfs_stats.total_files--;
    return 0;
}

/* === File operations (same as before but with RTC timestamps) === */

int32_t vfs_open(const char* name) { return vfs_open_mode(name, VFS_MODE_READ); }

int32_t vfs_open_mode(const char* name, uint8_t mode) {
    int fi = find_file(name);
    if (fi < 0 && (mode & VFS_MODE_CREATE)) {
        fi = vfs_create_empty(name);
        if (fi < 0) return -1;
        fi = find_file(name);
    }
    if (fi < 0) return -1;
    if (ft[fi].is_directory) return -4;
    if ((mode & VFS_MODE_WRITE) && ft[fi].readonly) return -3;

    int fd = -1;
    for (int i = 0; i < VFS_MAX_OPEN; i++)
        if (!fdt[i].used) { fd = i; break; }
    if (fd < 0) return -1;

    fdt[fd].file_idx = fi;
    fdt[fd].offset = (mode & VFS_MODE_APPEND) ? ft[fi].size : 0;
    fdt[fd].mode = mode;
    fdt[fd].used = true;
    ft[fi].open_count++;
    g_vfs_stats.open_fds++;
    g_vfs_stats.total_opens++;
    return fd;
}

int32_t vfs_read(int32_t fd, void* buf, uint32_t count) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fdt[fd].used) return -1;
    vfs_fd_t* d = &fdt[fd];
    vfs_file_t* f = &ft[d->file_idx];
    uint32_t rem = f->size - d->offset;
    if (!rem) return 0;
    if (count > rem) count = rem;
    memcpy(buf, f->data + d->offset, count);
    d->offset += count;
    f->read_count++;
    f->total_bytes_read += count;
    g_vfs_stats.total_reads++;
    g_vfs_stats.total_bytes_read += count;
    return (int32_t)count;
}

int32_t vfs_write(int32_t fd, const void* buf, uint32_t count) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fdt[fd].used) return -1;
    if (!(fdt[fd].mode & (VFS_MODE_WRITE | VFS_MODE_APPEND))) return -2;
    vfs_fd_t* d = &fdt[fd];
    vfs_file_t* f = &ft[d->file_idx];
    if (f->readonly) return -3;

    uint32_t end = d->offset + count;
    if (end > VFS_FILE_MAXSZ) { count = VFS_FILE_MAXSZ - d->offset; end = d->offset + count; }
    if (count == 0) return 0;

    if (end > f->capacity) {
        uint32_t new_cap = f->capacity * 2;
        if (new_cap < end) new_cap = end;
        if (new_cap > VFS_FILE_MAXSZ) new_cap = VFS_FILE_MAXSZ;
        uint8_t* new_buf = (uint8_t*)kmalloc(new_cap);
        if (!new_buf) return -1;
        memset(new_buf, 0, new_cap);
        memcpy(new_buf, f->data, f->size);
        kfree(f->data);
        f->data = new_buf;
        f->capacity = new_cap;
    }

    memcpy(f->data + d->offset, buf, count);
    d->offset += count;
    if (d->offset > f->size) {
        g_vfs_stats.total_size += (d->offset - f->size);
        f->size = d->offset;
    }
    f->write_count++;
    f->total_bytes_written += count;
    f->modified_tick = pit_get_ticks();
    f->modified_time = rtc_unix_approx();
    g_vfs_stats.total_writes++;
    g_vfs_stats.total_bytes_written += count;
    return (int32_t)count;
}

int32_t vfs_seek(int32_t fd, uint32_t offset) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fdt[fd].used) return -1;
    vfs_file_t* f = &ft[fdt[fd].file_idx];
    if (offset > f->size) offset = f->size;
    fdt[fd].offset = offset;
    return 0;
}

void vfs_close(int32_t fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || !fdt[fd].used) return;
    fdt[fd].used = false;
    g_vfs_stats.open_fds--;
}

int32_t vfs_delete(const char* name) {
    int fi = find_file(name);
    if (fi < 0) return -1;
    if (ft[fi].readonly) return -2;
    if (ft[fi].is_directory) return vfs_rmdir(name);
    if (ft[fi].open_count > 0) return -3;
    g_vfs_stats.total_size -= ft[fi].size;
    g_vfs_stats.total_files--;
    g_vfs_stats.total_deletes++;
    if (ft[fi].data) kfree(ft[fi].data);
    memset(&ft[fi], 0, sizeof(vfs_file_t));
    return 0;
}

int32_t vfs_rename(const char* old_name, const char* new_name) {
    int fi = find_file(old_name);
    if (fi < 0) return -1;
    if (ft[fi].readonly) return -2;
    if (find_file(new_name) >= 0) return -3;
    strcpy(ft[fi].name, new_name);
    ft[fi].modified_tick = pit_get_ticks();
    ft[fi].modified_time = rtc_unix_approx();
    return 0;
}

int32_t vfs_truncate(const char* name) {
    int fi = find_file(name);
    if (fi < 0) return -1;
    if (ft[fi].readonly) return -2;
    g_vfs_stats.total_size -= ft[fi].size;
    ft[fi].size = 0;
    memset(ft[fi].data, 0, ft[fi].capacity);
    ft[fi].modified_tick = pit_get_ticks();
    ft[fi].modified_time = rtc_unix_approx();
    return 0;
}

bool vfs_exists(const char* name) { return find_file(name) >= 0; }

int32_t vfs_filesize(const char* name) {
    int fi = find_file(name);
    if (fi < 0) return -1;
    return (int32_t)ft[fi].size;
}

const vfs_file_t* vfs_info(const char* name) {
    int fi = find_file(name);
    return fi < 0 ? NULL : &ft[fi];
}

const vfs_file_t* vfs_info_by_index(int index) {
    if (index < 0 || index >= VFS_MAX_FILES) return NULL;
    return &ft[index];
}

/* Format an RTC timestamp as HH:MM */
static void put_time_short(uint32_t unix_ts) {
    if (unix_ts == 0) { vga_puts("--:--"); return; }
    uint32_t time_of_day = unix_ts % 86400;
    uint32_t h = time_of_day / 3600;
    uint32_t m = (time_of_day % 3600) / 60;
    vga_putchar('0' + h / 10); vga_putchar('0' + h % 10);
    vga_putchar(':');
    vga_putchar('0' + m / 10); vga_putchar('0' + m % 10);
}

void vfs_list(void) {
    /* Show CWD */
    vga_puts_color("  pwd: ", VGA_DARK_GREY, VGA_BLACK);
    vga_puts_color(cwd, VGA_YELLOW, VGA_BLACK);
    vga_puts("\n");
    vga_puts_color("  Name               Size     R/W    Time\n", VGA_DARK_GREY, VGA_BLACK);
    vga_puts_color("  ----               ----     ---    ----\n", VGA_DARK_GREY, VGA_BLACK);

    /* Show directories first */
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!ft[i].used || !ft[i].is_directory) continue;
        if (strcmp(ft[i].name, "/") == 0) continue; /* Skip root */
        vfs_file_t* f = &ft[i];

        vga_puts_color("  [D] ", VGA_LIGHT_BLUE, VGA_BLACK);
        vga_puts_color(f->name, VGA_LIGHT_BLUE, VGA_BLACK);
        vga_puts("/");
        for (uint32_t j = strlen(f->name) + 1; j < 15; j++) vga_putchar(' ');
        vga_puts("<DIR>   ");
        vga_puts("         ");
        put_time_short(f->created_time);
        vga_puts("\n");
    }

    /* Then files */
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!ft[i].used || ft[i].is_directory) continue;
        vfs_file_t* f = &ft[i];

        const char* ext = f->name;
        while (*ext && *ext != '.') ext++;
        if (strcmp(ext, ".txt") == 0)
            vga_puts_color("  [T] ", VGA_LIGHT_GREEN, VGA_BLACK);
        else if (strcmp(ext, ".cfg") == 0)
            vga_puts_color("  [C] ", VGA_LIGHT_CYAN, VGA_BLACK);
        else if (strcmp(ext, ".elf") == 0)
            vga_puts_color("  [E] ", VGA_LIGHT_MAGENTA, VGA_BLACK);
        else
            vga_puts_color("  [F] ", VGA_DARK_GREY, VGA_BLACK);

        uint8_t nc = f->readonly ? VGA_DARK_GREY : VGA_WHITE;
        vga_puts_color(f->name, nc, VGA_BLACK);
        for (uint32_t j = strlen(f->name); j < 15; j++) vga_putchar(' ');

        if (f->size < 1024) { vga_put_dec(f->size); vga_puts("B"); }
        else { vga_put_dec(f->size / 1024); vga_puts("KB"); }
        uint32_t slen = 1, sv = f->size;
        while (sv >= 10) { slen++; sv /= 10; }
        for (uint32_t j = slen + 1; j < 8; j++) vga_putchar(' ');

        vga_put_dec(f->read_count); vga_puts("/"); vga_put_dec(f->write_count);
        vga_puts("    ");
        put_time_short(f->modified_time);
        if (f->readonly) vga_puts_color(" [RO]", VGA_DARK_GREY, VGA_BLACK);
        vga_puts("\n");
    }
}

void vfs_dump(void) {
    vga_puts("  VFS: ");
    vga_put_dec(g_vfs_stats.total_files); vga_puts(" files, ");
    vga_put_dec(g_vfs_stats.total_size); vga_puts("B total, ");
    vga_put_dec(g_vfs_stats.total_reads); vga_puts("R ");
    vga_put_dec(g_vfs_stats.total_writes); vga_puts("W ");
    vga_put_dec(g_vfs_stats.total_deletes); vga_puts("D\n");
}
