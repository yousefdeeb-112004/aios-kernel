#ifndef _KERNEL_PIC_H
#define _KERNEL_PIC_H
#include <kernel/types.h>
#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1
void pic_init(void); void pic_send_eoi(uint8_t irq);
void pic_unmask_irq(uint8_t irq); void pic_mask_irq(uint8_t irq);
#endif
