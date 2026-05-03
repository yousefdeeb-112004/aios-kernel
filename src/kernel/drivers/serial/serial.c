#include <drivers/serial.h>
#include <kernel/ports.h>
void serial_init(void){outb(SERIAL_COM1+1,0);outb(SERIAL_COM1+3,0x80);outb(SERIAL_COM1,3);outb(SERIAL_COM1+1,0);outb(SERIAL_COM1+3,0x03);outb(SERIAL_COM1+2,0xC7);outb(SERIAL_COM1+4,0x0B);}
void serial_putchar(char c){while((inb(SERIAL_COM1+5)&0x20)==0);outb(SERIAL_COM1,(uint8_t)c);}
void serial_puts(const char* s){while(*s){if(*s=='\n')serial_putchar('\r');serial_putchar(*s++);}}
void serial_put_hex(uint32_t v){const char h[]="0123456789ABCDEF";serial_puts("0x");for(int i=28;i>=0;i-=4)serial_putchar(h[(v>>i)&0xF]);}
void serial_put_dec(uint32_t v){if(v==0){serial_putchar('0');return;}char b[12];int i=0;while(v>0){b[i++]='0'+(v%10);v/=10;}while(i>0)serial_putchar(b[--i]);}
