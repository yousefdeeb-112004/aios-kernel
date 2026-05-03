/* =============================================================================
 * Kernel Panic — Full-Screen Diagnostic Display
 *
 * Writes directly to VGA buffer (0xB8000) to avoid relying on any
 * kernel subsystem that might be corrupted.
 * ============================================================================= */

#include <kernel/panic.h>
#include <kernel/ports.h>
#include <drivers/serial.h>
#include <lib/string.h>

/* Direct VGA access — don't use VGA driver (might be corrupted) */
#define PANIC_VGA  ((uint16_t*)0xB8000)
#define PANIC_W    80
#define PANIC_H    25
#define PC(fg,bg)  ((uint8_t)((fg)|((bg)<<4)))

static int panic_x, panic_y;
static uint8_t panic_color;

static void p_cell(int x, int y, char c, uint8_t col) {
    if (x >= 0 && x < PANIC_W && y >= 0 && y < PANIC_H)
        PANIC_VGA[y * PANIC_W + x] = (uint16_t)((uint8_t)c) | ((uint16_t)col << 8);
}

static void p_clear(uint8_t col) {
    for (int i = 0; i < PANIC_W * PANIC_H; i++)
        PANIC_VGA[i] = (uint16_t)' ' | ((uint16_t)col << 8);
    panic_x = 0; panic_y = 0;
}

static void p_setcolor(uint8_t col) { panic_color = col; }

static void p_putchar(char c) {
    if (c == '\n') { panic_x = 0; panic_y++; return; }
    if (panic_y >= PANIC_H) return;
    p_cell(panic_x, panic_y, c, panic_color);
    panic_x++;
    if (panic_x >= PANIC_W) { panic_x = 0; panic_y++; }
}

static void p_puts(const char* s) {
    while (*s) p_putchar(*s++);
}

static void p_puthex(uint32_t v) {
    const char h[] = "0123456789ABCDEF";
    p_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        p_putchar(h[(v >> i) & 0xF]);
}

static void p_putdec(uint32_t v) {
    if (v == 0) { p_putchar('0'); return; }
    char b[12]; int i = 0;
    while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) p_putchar(b[--i]);
}

static void p_fill_row(int y, uint8_t col) {
    for (int x = 0; x < PANIC_W; x++)
        p_cell(x, y, ' ', col);
}

/* Exception names */
static const char* exc_names[] = {
    "Divide by Zero",        "Debug",
    "Non-Maskable Interrupt", "Breakpoint",
    "Overflow",              "Bound Range Exceeded",
    "Invalid Opcode",        "Device Not Available",
    "Double Fault",          "Coprocessor Segment",
    "Invalid TSS",           "Segment Not Present",
    "Stack-Segment Fault",   "General Protection Fault",
    "Page Fault",            "Reserved",
    "x87 FP Exception",     "Alignment Check",
    "Machine Check",         "SIMD FP Exception",
    "Virtualization",        "Control Protection",
    "Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved"
};

/* Walk the stack using frame pointers (EBP chain) */
static void p_backtrace(uint32_t ebp, uint32_t eip) {
    p_setcolor(PC(14, 4));
    p_puts("  Call trace:\n");
    p_setcolor(PC(15, 4));

    /* First frame: the faulting EIP */
    if (eip) {
        p_puts("    #0  ");
        p_puthex(eip);
        p_puts("  <-- fault\n");
    }

    /* Walk EBP chain */
    uint32_t* frame = (uint32_t*)ebp;
    for (int depth = 1; depth < 10; depth++) {
        /* Sanity: frame pointer must be in reasonable kernel range */
        if ((uint32_t)frame < 0x100000 || (uint32_t)frame > 0x01000000)
            break;
        /* frame[0] = previous EBP, frame[1] = return address */
        uint32_t ret_addr = frame[1];
        uint32_t prev_ebp = frame[0];
        if (ret_addr == 0) break;

        p_puts("    #");
        p_putdec(depth);
        if (depth < 10) p_puts(" ");
        p_puts(" ");
        p_puthex(ret_addr);
        p_puts("\n");

        frame = (uint32_t*)prev_ebp;
    }
}

/* Main panic display from exception */
void kpanic_exception(registers_t* regs) {
    __asm__ volatile("cli");

    /* Red screen of death */
    p_clear(PC(15, 4));

    /* Header bar */
    p_fill_row(0, PC(15, 4));
    panic_x = 0; panic_y = 0;
    p_setcolor(PC(15, 4));
    p_puts("  AIOS KERNEL PANIC");

    /* Exception info */
    panic_x = 0; panic_y = 2;
    p_setcolor(PC(14, 4));
    p_puts("  Exception: ");
    p_setcolor(PC(15, 4));
    if (regs->int_no < 32)
        p_puts(exc_names[regs->int_no]);
    else {
        p_puts("IRQ "); p_putdec(regs->int_no);
    }
    p_puts(" (#"); p_putdec(regs->int_no); p_puts(")\n");

    /* Error code */
    p_setcolor(PC(14, 4));
    p_puts("  Error code: ");
    p_setcolor(PC(15, 4));
    p_puthex(regs->err_code);
    p_puts("\n");

    /* Page fault: show CR2 and decode error bits */
    if (regs->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        p_setcolor(PC(14, 4));
        p_puts("  Fault addr: ");
        p_setcolor(PC(15, 4));
        p_puthex(cr2);
        p_puts(" (");
        p_puts((regs->err_code & 1) ? "protection" : "not-present");
        p_puts(", ");
        p_puts((regs->err_code & 2) ? "write" : "read");
        p_puts(", ");
        p_puts((regs->err_code & 4) ? "user" : "kernel");
        p_puts(")\n");
    }

    /* Register dump */
    panic_x = 0; panic_y = 7;
    p_setcolor(PC(14, 4));
    p_puts("  Registers:\n");
    p_setcolor(PC(15, 4));

    p_puts("    EAX="); p_puthex(regs->eax);
    p_puts("  EBX="); p_puthex(regs->ebx);
    p_puts("  ECX="); p_puthex(regs->ecx);
    p_puts("  EDX="); p_puthex(regs->edx);
    p_puts("\n");

    p_puts("    ESI="); p_puthex(regs->esi);
    p_puts("  EDI="); p_puthex(regs->edi);
    p_puts("  EBP="); p_puthex(regs->ebp);
    p_puts("  ESP="); p_puthex(regs->esp);
    p_puts("\n");

    p_puts("    EIP="); p_puthex(regs->eip);
    p_puts("  CS="); p_puthex(regs->cs);
    p_puts("  EFLAGS="); p_puthex(regs->eflags);
    p_puts("\n");

    p_puts("    DS="); p_puthex(regs->ds);
    p_puts("  SS="); p_puthex(regs->ss);
    p_puts("\n");

    /* Stack trace */
    panic_x = 0; panic_y = 13;
    p_backtrace(regs->ebp, regs->eip);

    /* Raw stack dump (16 dwords from ESP) */
    panic_x = 0; panic_y = 20;
    p_setcolor(PC(14, 4));
    p_puts("  Stack (ESP ");
    p_puthex(regs->esp);
    p_puts("):\n");
    p_setcolor(PC(7, 4));
    uint32_t* sp = (uint32_t*)regs->esp;
    p_puts("    ");
    for (int i = 0; i < 16; i++) {
        if ((uint32_t)&sp[i] < 0x100000 || (uint32_t)&sp[i] > 0x01000000)
            break;
        p_puthex(sp[i]);
        p_putchar(' ');
        if (i == 7) { p_puts("\n    "); }
    }

    /* Footer */
    panic_x = 0; panic_y = PANIC_H - 1;
    p_setcolor(PC(0, 7));
    p_fill_row(PANIC_H - 1, PC(0, 7));
    panic_x = 1; panic_y = PANIC_H - 1;
    p_puts("  System halted. Reboot to continue. | Use addr2line for EIP lookup.");

    /* Also dump to serial for logging */
    serial_puts("\n=== KERNEL PANIC ===\n");
    serial_puts("Exception: ");
    if (regs->int_no < 32) serial_puts(exc_names[regs->int_no]);
    serial_puts("\nEIP="); serial_put_hex(regs->eip);
    serial_puts(" ERR="); serial_put_hex(regs->err_code);
    if (regs->int_no == 14) {
        uint32_t cr2; __asm__ volatile("mov %%cr2, %0":"=r"(cr2));
        serial_puts(" CR2="); serial_put_hex(cr2);
    }
    serial_puts("\nEAX="); serial_put_hex(regs->eax);
    serial_puts(" EBX="); serial_put_hex(regs->ebx);
    serial_puts(" ECX="); serial_put_hex(regs->ecx);
    serial_puts(" EDX="); serial_put_hex(regs->edx);
    serial_puts("\n=== END PANIC ===\n");

    /* Halt forever */
    for (;;) __asm__ volatile("cli; hlt");
}

/* General kpanic for use in kernel code */
void kpanic(const char* message) {
    __asm__ volatile("cli");

    p_clear(PC(15, 4));
    p_fill_row(0, PC(15, 4));
    panic_x = 0; panic_y = 0;
    p_setcolor(PC(15, 4));
    p_puts("  AIOS KERNEL PANIC");

    panic_x = 0; panic_y = 2;
    p_setcolor(PC(14, 4));
    p_puts("  Reason: ");
    p_setcolor(PC(15, 4));
    p_puts(message);
    p_puts("\n");

    /* Stack trace from current EBP */
    uint32_t ebp;
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));
    panic_x = 0; panic_y = 5;
    p_backtrace(ebp, 0);

    /* Serial */
    serial_puts("\n=== KERNEL PANIC ===\n");
    serial_puts(message);
    serial_puts("\n=== END PANIC ===\n");

    /* Footer */
    panic_x = 0; panic_y = PANIC_H - 1;
    p_setcolor(PC(0, 7));
    p_fill_row(PANIC_H - 1, PC(0, 7));
    panic_x = 1; panic_y = PANIC_H - 1;
    p_puts("  System halted. Reboot to continue.");

    for (;;) __asm__ volatile("cli; hlt");
}
