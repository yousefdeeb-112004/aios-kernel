/* =============================================================================
 * PCI Bus Driver
 *
 * Enumerates PCI devices by reading Configuration Space via I/O ports
 * 0xCF8 (address) and 0xCFC (data).
 *
 * Needed to find the network card (and any other PCI devices).
 * ============================================================================= */
#ifndef _KERNEL_PCI_H
#define _KERNEL_PCI_H

#include <kernel/types.h>

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC
#define PCI_MAX_DEVICES  32

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint32_t bar[6];        /* Base Address Registers */
    uint8_t  irq_line;
    bool     present;
} pci_device_t;

typedef struct {
    pci_device_t devices[PCI_MAX_DEVICES];
    uint32_t count;
} pci_bus_t;

extern pci_bus_t g_pci;

/* Read 32-bit value from PCI config space */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* Write 32-bit value to PCI config space */
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

/* Scan PCI bus and populate g_pci */
void pci_init(void);

/* Find device by vendor+device ID. Returns NULL if not found. */
pci_device_t* pci_find(uint16_t vendor_id, uint16_t device_id);

/* Find device by class+subclass. Returns NULL if not found. */
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);

/* Enable PCI bus mastering for a device (needed for DMA) */
void pci_enable_bus_master(pci_device_t* dev);

/* Print PCI device list */
void pci_dump(void);

/* Get class name string */
const char* pci_class_name(uint8_t class_code, uint8_t subclass);

#endif
