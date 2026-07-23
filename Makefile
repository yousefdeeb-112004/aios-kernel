CC  = gcc
AS  = gcc
LD  = ld
CFLAGS  = -m32 -std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -ffreestanding -fno-builtin -fno-stack-protector -fno-omit-frame-pointer -nostdlib -nostdinc -mno-red-zone -Wall -Wextra -Wno-unused-parameter -Iinclude -c -g -DAI_FEATURES
ASFLAGS = -m32 -c -g
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib
BUILD = build

# === USER PROGRAM (built first, embedded in kernel VFS) ===
USER_ELF = $(BUILD)/user/hello.elf
USER_ELF_DATA = $(BUILD)/user/hello_elf_data.c
USER_ELF_OBJ  = $(BUILD)/user/hello_elf_data.o

# === libaios: freestanding C runtime for Ring 3 programs ===
# Compiled with a COMPLETELY SEPARATE include path: -Isrc/user/lib and nothing
# else. User code must never see include/ (the kernel headers) — the isolation
# is enforced by the compiler here rather than by convention.
#   -Os          keep the image small and stack frames shallow (one 4KB stack)
#   -mno-sse/mmx never emit vector instructions; Ring 3 cannot rely on CR4 state
#   -fno-pic     absolute addressing, matching the fixed load base in user.ld
USER_CFLAGS = -m32 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
              -nostdlib -nostdinc -mno-red-zone -mno-sse -mno-mmx -fno-pic \
              -fno-asynchronous-unwind-tables -Wall -Wextra \
              -Wno-unused-parameter -Isrc/user/lib -Os -c

LIBAIOS_SOURCES = src/user/lib/syscalls.c src/user/lib/string.c \
                  src/user/lib/malloc.c
LIBAIOS_OBJECTS = $(patsubst src/user/lib/%.c, $(BUILD)/user/lib/%.o, $(LIBAIOS_SOURCES))
CRT0_OBJECT     = $(BUILD)/user/lib/crt0.o

UCTEST_ELF      = $(BUILD)/user/uctest.elf
UCTEST_ELF_DATA = $(BUILD)/user/uctest_elf_data.c
UCTEST_ELF_OBJ  = $(BUILD)/user/uctest_elf_data.o

NAWA_ELF        = $(BUILD)/user/nawa.elf
NAWA_ELF_DATA   = $(BUILD)/user/nawa_elf_data.c
NAWA_ELF_OBJ    = $(BUILD)/user/nawa_elf_data.o

# === KERNEL SOURCES ===
ASM_SOURCES = src/boot/boot.S src/boot/gdt.S src/kernel/arch/x86_64/isr.S \
              src/kernel/proc/context.S src/kernel/proc/usermode_asm.S \
              src/kernel/smp/ap_trampoline.S

C_SOURCES = src/kernel/core/kmain.c src/kernel/core/boot_info.c src/kernel/core/log.c \
            src/kernel/core/panic.c \
            src/kernel/arch/x86_64/cpu.c src/kernel/arch/x86_64/idt.c src/kernel/arch/x86_64/pic.c \
            src/kernel/arch/x86_64/gdt.c \
            src/kernel/drivers/vga/vga.c src/kernel/drivers/vga/vga_gfx.c \
            src/kernel/drivers/serial/serial.c \
            src/kernel/drivers/timer/pit.c src/kernel/drivers/keyboard/keyboard.c \
            src/kernel/drivers/mouse/mouse.c \
            src/kernel/drivers/ata/ata.c \
            src/kernel/drivers/rtc/rtc.c \
            src/kernel/mm/pmm.c src/kernel/mm/vmm.c src/kernel/mm/heap.c \
            src/kernel/proc/process.c src/kernel/proc/usermode.c \
            src/kernel/proc/elf_loader.c \
            src/kernel/syscall/syscall.c \
            src/kernel/fs/vfs.c src/kernel/fs/editor.c src/kernel/ai/event_bus.c \
            src/kernel/ai/agent/agent.c \
            src/kernel/net/pci.c src/kernel/net/net.c \
            src/kernel/wm/wm.c \
            src/kernel/smp/smp.c \
            src/kernel/fs/aios_fs.c \
            src/kernel/dev/dev.c \
            src/kernel/ipc/pipe.c \
            src/kernel/devtrack/devtrack.c src/kernel/shell/shell.c \
            src/lib/string.c src/lib/kprintf.c

ASM_OBJECTS = $(patsubst src/%.S, $(BUILD)/%.o, $(ASM_SOURCES))
C_OBJECTS   = $(patsubst src/%.c, $(BUILD)/%.o, $(C_SOURCES))
OBJECTS     = $(ASM_OBJECTS) $(C_OBJECTS) $(USER_ELF_OBJ) $(UCTEST_ELF_OBJ) \
              $(NAWA_ELF_OBJ)

.PHONY: all run clean

all: $(BUILD)/aios-kernel.bin
	@echo ""
	@echo "AIOS Kernel v2.0 built! (61 cmds, TCP, DNS, pipes, waitpid, fd-table)"
	@echo "Run: make run"
	@echo "New: ajhz | dev r/w <name> | tnsyq | hfzk | hmml | dskfs | aqtl | khlf | fzh"

# === STEP 1: Build user program ELF ===
$(BUILD)/user/hello.o: src/user/hello.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_ELF): $(BUILD)/user/hello.o src/user/user.ld
	$(LD) -m elf_i386 -T src/user/user.ld -nostdlib -o $@ $(BUILD)/user/hello.o

# === STEP 2: Convert ELF to C byte array ===
$(USER_ELF_DATA): $(USER_ELF)
	@python3 scripts/elf2c.py $(USER_ELF) $@

$(USER_ELF_OBJ): $(USER_ELF_DATA)
	$(CC) $(CFLAGS) $(USER_ELF_DATA) -o $@

# === STEP 1b: Build libaios + the C demo program (uctest.elf) ===
# crt0 goes first on the link line so _start lands at the load base; ENTRY() in
# user.ld would find it either way, but keeping it first makes the layout
# predictable when reading a disassembly.
$(CRT0_OBJECT): src/user/lib/crt0.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/user/lib/%.o: src/user/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $< -o $@

$(BUILD)/user/demo/uctest.o: src/user/demo/uctest.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $< -o $@

$(UCTEST_ELF): $(CRT0_OBJECT) $(BUILD)/user/demo/uctest.o $(LIBAIOS_OBJECTS) src/user/user.ld
	$(LD) -m elf_i386 -T src/user/user.ld -nostdlib -o $@ \
	      $(CRT0_OBJECT) $(BUILD)/user/demo/uctest.o $(LIBAIOS_OBJECTS)

$(UCTEST_ELF_DATA): $(UCTEST_ELF)
	@python3 scripts/elf2c.py $(UCTEST_ELF) $@ user_uctest_elf

$(UCTEST_ELF_OBJ): $(UCTEST_ELF_DATA)
	$(CC) $(CFLAGS) $(UCTEST_ELF_DATA) -o $@

# === STEP 1c: Build nawa, the Forth-style interpreter (also on libaios) ===
$(BUILD)/user/demo/nawa.o: src/user/demo/nawa.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $< -o $@

$(NAWA_ELF): $(CRT0_OBJECT) $(BUILD)/user/demo/nawa.o $(LIBAIOS_OBJECTS) src/user/user.ld
	$(LD) -m elf_i386 -T src/user/user.ld -nostdlib -o $@ \
	      $(CRT0_OBJECT) $(BUILD)/user/demo/nawa.o $(LIBAIOS_OBJECTS)

$(NAWA_ELF_DATA): $(NAWA_ELF)
	@python3 scripts/elf2c.py $(NAWA_ELF) $@ user_nawa_elf

$(NAWA_ELF_OBJ): $(NAWA_ELF_DATA)
	$(CC) $(CFLAGS) $(NAWA_ELF_DATA) -o $@

# === STEP 3: Build kernel (depends on user ELF data) ===
$(BUILD)/aios-kernel.bin: $(OBJECTS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

$(BUILD)/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

# vfs.c depends on both embedded ELFs being generated first
$(BUILD)/kernel/fs/vfs.o: $(USER_ELF_DATA) $(UCTEST_ELF_DATA) $(NAWA_ELF_DATA)

run: $(BUILD)/aios-kernel.bin disk.img
	qemu-system-i386 -kernel $(BUILD)/aios-kernel.bin -m 128M -no-reboot -no-shutdown -serial stdio -hda disk.img -device rtl8139,netdev=net0 -netdev user,id=net0 -smp 4

# Create a 1MB disk image if it doesn't exist
disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=1 2>/dev/null
	@echo "Created 1MB disk.img"

clean:
	rm -rf $(BUILD)
