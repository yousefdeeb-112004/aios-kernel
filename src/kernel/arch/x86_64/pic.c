#include <kernel/pic.h>
#include <kernel/ports.h>
void pic_init(void){outb(PIC1_CMD,0x11);io_wait();outb(PIC2_CMD,0x11);io_wait();outb(PIC1_DATA,0x20);io_wait();outb(PIC2_DATA,0x28);io_wait();outb(PIC1_DATA,0x04);io_wait();outb(PIC2_DATA,0x02);io_wait();outb(PIC1_DATA,0x01);io_wait();outb(PIC2_DATA,0x01);io_wait();outb(PIC1_DATA,0xFF);outb(PIC2_DATA,0xFF);}
void pic_send_eoi(uint8_t irq){if(irq>=8)outb(PIC2_CMD,0x20);outb(PIC1_CMD,0x20);}
void pic_unmask_irq(uint8_t irq){uint16_t p=(irq<8)?PIC1_DATA:PIC2_DATA;if(irq>=8)irq-=8;outb(p,inb(p)&~(1<<irq));}
void pic_mask_irq(uint8_t irq){uint16_t p=(irq<8)?PIC1_DATA:PIC2_DATA;if(irq>=8)irq-=8;outb(p,inb(p)|(1<<irq));}
