/* PCI Bus Scanner — reads Configuration Space via ports 0xCF8/0xCFC */

#include <kernel/pci.h>
#include <kernel/ports.h>
#include <drivers/vga.h>
#include <lib/string.h>

pci_bus_t g_pci;

static inline uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint32_t)(0x80000000 | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
            ((uint32_t)func << 8) | (off & 0xFC));
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outb(PCI_CONFIG_ADDR, 0); /* Force 32-bit write */
    __asm__ volatile("outl %0, %1" : : "a"(pci_addr(bus, slot, func, off)),
                     "Nd"((uint16_t)PCI_CONFIG_ADDR));
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)PCI_CONFIG_DATA));
    return val;
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(pci_addr(bus, slot, func, off)),
                     "Nd"((uint16_t)PCI_CONFIG_ADDR));
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"((uint16_t)PCI_CONFIG_DATA));
}

void pci_init(void) {
    memset(&g_pci, 0, sizeof(pci_bus_t));

    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            /* Check function 0 first */
            uint32_t reg0 = pci_read32(bus, slot, 0, 0x00);
            uint16_t vendor = (uint16_t)(reg0 & 0xFFFF);
            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            /* Check if multi-function: header type bit 7 */
            uint32_t hdr = pci_read32(bus, slot, 0, 0x0C);
            uint8_t header_type = (uint8_t)((hdr >> 16) & 0xFF);
            int max_func = (header_type & 0x80) ? 8 : 1;

            for (int func = 0; func < max_func; func++) {
                reg0 = pci_read32(bus, slot, func, 0x00);
                vendor = (uint16_t)(reg0 & 0xFFFF);
                uint16_t device = (uint16_t)(reg0 >> 16);
                if (vendor == 0xFFFF || vendor == 0x0000) continue;
                if (g_pci.count >= PCI_MAX_DEVICES) break;

                pci_device_t* d = &g_pci.devices[g_pci.count];
                d->present = true;
                d->bus = bus;
                d->slot = slot;
                d->func = func;
                d->vendor_id = vendor;
                d->device_id = device;

                uint32_t reg2 = pci_read32(bus, slot, func, 0x08);
                d->revision = (uint8_t)(reg2 & 0xFF);
                d->prog_if = (uint8_t)((reg2 >> 8) & 0xFF);
                d->subclass = (uint8_t)((reg2 >> 16) & 0xFF);
                d->class_code = (uint8_t)((reg2 >> 24) & 0xFF);

                for (int b = 0; b < 6; b++)
                    d->bar[b] = pci_read32(bus, slot, func, 0x10 + b * 4);

                uint32_t reg3c = pci_read32(bus, slot, func, 0x3C);
                d->irq_line = (uint8_t)(reg3c & 0xFF);

                g_pci.count++;
            }
        }
    }
}

pci_device_t* pci_find(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < g_pci.count; i++) {
        if (g_pci.devices[i].vendor_id == vendor &&
            g_pci.devices[i].device_id == device)
            return &g_pci.devices[i];
    }
    return NULL;
}

pci_device_t* pci_find_class(uint8_t cls, uint8_t sub) {
    for (uint32_t i = 0; i < g_pci.count; i++) {
        if (g_pci.devices[i].class_code == cls &&
            g_pci.devices[i].subclass == sub)
            return &g_pci.devices[i];
    }
    return NULL;
}

void pci_enable_bus_master(pci_device_t* dev) {
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 2);  /* Set Bus Master bit */
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

const char* pci_class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
        case 0x00: return "Unclassified";
        case 0x01:
            switch (sub) {
                case 0x01: return "IDE Controller";
                case 0x06: return "SATA Controller";
                default: return "Storage";
            }
        case 0x02:
            switch (sub) {
                case 0x00: return "Ethernet";
                case 0x80: return "Network Other";
                default: return "Network";
            }
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06:
            switch (sub) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x80: return "Other Bridge";
                default: return "Bridge";
            }
        case 0x07: return "Communication";
        case 0x08: return "System";
        case 0x0C: return "Serial Bus";
        default: return "Other";
    }
}

void pci_dump(void) {
    vga_puts_color("=== PCI Devices ===\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  Bus:Slot.Fn Vendor:Device Class         IRQ\n");
    for (uint32_t i = 0; i < g_pci.count; i++) {
        pci_device_t* d = &g_pci.devices[i];
        vga_puts("  ");
        vga_put_dec(d->bus); vga_puts(":");
        vga_put_dec(d->slot); vga_puts(".");
        vga_put_dec(d->func);
        if (d->slot < 10) vga_puts(" ");
        vga_puts("  ");
        vga_put_hex(d->vendor_id); vga_puts(":");
        vga_put_hex(d->device_id);
        vga_puts(" ");
        const char* name = pci_class_name(d->class_code, d->subclass);
        vga_puts_color(name, VGA_YELLOW, VGA_BLACK);
        for (uint32_t j = strlen(name); j < 16; j++) vga_putchar(' ');
        vga_put_dec(d->irq_line);
        vga_puts("\n");
    }
    vga_puts("  Total: "); vga_put_dec(g_pci.count); vga_puts(" devices\n");
}
