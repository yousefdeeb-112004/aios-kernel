#include <kernel/pmm.h>
#include <kernel/multiboot.h>
#include <drivers/vga.h>
#include <lib/string.h>
#define MAX_PAGES 32768
#define BITMAP_SIZE (MAX_PAGES/8)
static uint8_t bitmap[BITMAP_SIZE]; pmm_stats_t g_pmm_stats;
static inline void bset(uint32_t p){bitmap[p/8]|=(1<<(p%8));} static inline void bclr(uint32_t p){bitmap[p/8]&=~(1<<(p%8));} static inline bool btst(uint32_t p){return(bitmap[p/8]>>(p%8))&1;}
void pmm_init(uint32_t ma,uint32_t ml,uint32_t ks,uint32_t ke){memset(bitmap,0xFF,BITMAP_SIZE);memset(&g_pmm_stats,0,sizeof(pmm_stats_t));uint32_t avail=0,off=0;while(off<ml){multiboot_mmap_entry_t*e=(multiboot_mmap_entry_t*)(ma+off);if(e->type==MMAP_TYPE_AVAILABLE){uint32_t b=(uint32_t)e->base_addr;uint32_t l=(uint32_t)e->length;if(b%PAGE_SIZE){uint32_t a=PAGE_SIZE-(b%PAGE_SIZE);if(a>l){off+=e->size+sizeof(e->size);continue;}b+=a;l-=a;}uint32_t ps=b/PAGE_SIZE;uint32_t pc=l/PAGE_SIZE;for(uint32_t i=0;i<pc&&(ps+i)<MAX_PAGES;i++){bclr(ps+i);avail++;}}off+=e->size+sizeof(e->size);}uint32_t rp=(1024*1024)/PAGE_SIZE;for(uint32_t i=0;i<rp&&i<MAX_PAGES;i++){if(!btst(i)){bset(i);avail--;}}uint32_t kps=ks/PAGE_SIZE;uint32_t kpe=(ke+PAGE_SIZE-1)/PAGE_SIZE;for(uint32_t i=kps;i<kpe&&i<MAX_PAGES;i++){if(!btst(i)){bset(i);avail--;}}g_pmm_stats.total_pages=avail+kpe-kps+rp;g_pmm_stats.free_pages=avail;g_pmm_stats.used_pages=g_pmm_stats.total_pages-avail;g_pmm_stats.peak_used=g_pmm_stats.used_pages;}
uint32_t pmm_alloc_page(void){for(uint32_t i=0;i<MAX_PAGES;i++){if(!btst(i)){bset(i);g_pmm_stats.used_pages++;g_pmm_stats.free_pages--;g_pmm_stats.alloc_count++;if(g_pmm_stats.used_pages>g_pmm_stats.peak_used)g_pmm_stats.peak_used=g_pmm_stats.used_pages;return i*PAGE_SIZE;}}return 0;}
void pmm_free_page(uint32_t a){uint32_t p=a/PAGE_SIZE;if(p>=MAX_PAGES||!btst(p))return;bclr(p);g_pmm_stats.used_pages--;g_pmm_stats.free_pages++;g_pmm_stats.free_count++;}
bool pmm_is_page_used(uint32_t a){return btst(a/PAGE_SIZE);}
void pmm_dump(void){vga_puts("  PMM: ");vga_put_dec(g_pmm_stats.free_pages);vga_puts("/");vga_put_dec(g_pmm_stats.total_pages);vga_puts(" free (");vga_put_dec((g_pmm_stats.free_pages*4)/1024);vga_puts("MB)\n");}
