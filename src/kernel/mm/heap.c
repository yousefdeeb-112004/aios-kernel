#include <kernel/heap.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/lock.h>
#include <lib/string.h>
#include <drivers/vga.h>
#define HEAP_START 0x00800000
#define HEAP_MAGIC 0xDEAD1234
#define HEADER_SIZE 8
typedef struct bh{uint32_t magic;uint32_t size;}bh_t;
typedef struct fb{bh_t header;struct fb*next;}fb_t;
heap_stats_t g_heap_stats; static fb_t*free_list=NULL; static uint32_t heap_end=HEAP_START;
static klock_t heap_lock = KLOCK_INIT;
static bool hexp(size_t min){size_t pn=(min+PAGE_SIZE-1)/PAGE_SIZE;for(size_t i=0;i<pn;i++){uint32_t p=pmm_alloc_page();if(!p)return false;vmm_map_page(heap_end,p,PTE_PRESENT|PTE_WRITABLE|PTE_USER);heap_end+=PAGE_SIZE;g_heap_stats.heap_size+=PAGE_SIZE;}return true;}
void heap_init(void){memset(&g_heap_stats,0,sizeof(heap_stats_t));free_list=NULL;heap_end=HEAP_START;hexp(64*1024);free_list=(fb_t*)HEAP_START;free_list->header.magic=HEAP_MAGIC;free_list->header.size=g_heap_stats.heap_size-HEADER_SIZE;free_list->next=NULL;}
void*kmalloc(size_t size){if(!size)return NULL;klock_acquire(&heap_lock);size=(size+3)&~3;if(size<sizeof(fb_t)-HEADER_SIZE)size=sizeof(fb_t)-HEADER_SIZE;fb_t*prev=NULL,*cur=free_list;while(cur){if(cur->header.size>=size){if(cur->header.size>=size+HEADER_SIZE+16){fb_t*nf=(fb_t*)((uint8_t*)cur+HEADER_SIZE+size);nf->header.magic=HEAP_MAGIC;nf->header.size=cur->header.size-size-HEADER_SIZE;nf->next=cur->next;cur->header.size=size;if(prev)prev->next=nf;else free_list=nf;}else{if(prev)prev->next=cur->next;else free_list=cur->next;}g_heap_stats.alloc_count++;g_heap_stats.total_allocated+=cur->header.size;klock_release(&heap_lock);return(void*)((uint8_t*)cur+HEADER_SIZE);}prev=cur;cur=cur->next;}if(!hexp(size+HEADER_SIZE+PAGE_SIZE)){klock_release(&heap_lock);return NULL;}fb_t*nb=(fb_t*)(heap_end-((size+HEADER_SIZE+PAGE_SIZE-1)/PAGE_SIZE)*PAGE_SIZE);nb->header.magic=HEAP_MAGIC;nb->header.size=heap_end-(uint32_t)nb-HEADER_SIZE;nb->next=free_list;free_list=nb;klock_release(&heap_lock);return kmalloc(size);}
void kfree(void*ptr){
    if(!ptr)return;
    klock_acquire(&heap_lock);
    bh_t*h=(bh_t*)((uint8_t*)ptr-HEADER_SIZE);
    if(h->magic!=HEAP_MAGIC){klock_release(&heap_lock);return;}
    g_heap_stats.free_count++;
    g_heap_stats.total_freed+=h->size;
    g_heap_stats.total_allocated-=h->size;
    fb_t*b=(fb_t*)h;

    /* Insert into free list sorted by address (ascending) */
    fb_t*prev=NULL,*cur=free_list;
    while(cur && cur<b){prev=cur;cur=cur->next;}
    /* Now: prev < b < cur (or edges are NULL) */

    b->next=cur;
    if(prev) prev->next=b;
    else free_list=b;

    /* Coalesce with NEXT block if adjacent */
    if(b->next){
        uint8_t*b_end=(uint8_t*)b+HEADER_SIZE+b->header.size;
        if(b_end==(uint8_t*)b->next){
            b->header.size+=HEADER_SIZE+b->next->header.size;
            b->next=b->next->next;
            g_heap_stats.coalesces++;
        }
    }

    /* Coalesce with PREVIOUS block if adjacent */
    if(prev){
        uint8_t*prev_end=(uint8_t*)prev+HEADER_SIZE+prev->header.size;
        if(prev_end==(uint8_t*)b){
            prev->header.size+=HEADER_SIZE+b->header.size;
            prev->next=b->next;
            g_heap_stats.coalesces++;
        }
    }

    klock_release(&heap_lock);
}
void*kmalloc_aligned(size_t s){void*p=kmalloc(s+PAGE_SIZE);if(!p)return NULL;return(void*)(((uint32_t)p+PAGE_SIZE-1)&~(PAGE_SIZE-1));}
void heap_dump(void){
    /* Count free blocks and find largest */
    uint32_t fb_count=0,largest=0;
    fb_t*cur=free_list;
    while(cur){fb_count++;if(cur->header.size>largest)largest=cur->header.size;cur=cur->next;}
    g_heap_stats.free_blocks=fb_count;
    g_heap_stats.largest_free=largest;
    vga_puts("  Heap: ");vga_put_dec(g_heap_stats.heap_size/1024);vga_puts("KB");
    vga_puts(" alloc:");vga_put_dec(g_heap_stats.alloc_count);
    vga_puts(" free:");vga_put_dec(g_heap_stats.free_count);
    vga_puts(" inuse:");vga_put_dec(g_heap_stats.total_allocated);vga_puts("B\n");
    vga_puts("  Frag: ");vga_put_dec(fb_count);vga_puts(" free blocks");
    vga_puts(" largest:");vga_put_dec(largest);vga_puts("B");
    vga_puts(" coalesces:");vga_put_dec(g_heap_stats.coalesces);vga_puts("\n");
}
