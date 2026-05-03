#include <lib/string.h>
void* memset(void* d, int v, size_t c) { uint8_t* p=(uint8_t*)d; for(size_t i=0;i<c;i++) p[i]=(uint8_t)v; return d; }
void* memcpy(void* d, const void* s, size_t c) { uint8_t* dp=(uint8_t*)d; const uint8_t* sp=(const uint8_t*)s; for(size_t i=0;i<c;i++) dp[i]=sp[i]; return d; }
int memcmp(const void* a, const void* b, size_t c) { const uint8_t* pa=(const uint8_t*)a; const uint8_t* pb=(const uint8_t*)b; for(size_t i=0;i<c;i++) if(pa[i]!=pb[i]) return pa[i]-pb[i]; return 0; }
size_t strlen(const char* s) { size_t l=0; while(s[l]) l++; return l; }
int strcmp(const char* a, const char* b) { while(*a&&*a==*b){a++;b++;} return *(unsigned char*)a-*(unsigned char*)b; }
char* strcpy(char* d, const char* s) { char* p=d; while(*s) *p++=*s++; *p='\0'; return d; }
int strncmp(const char* a, const char* b, size_t n) { for(size_t i=0;i<n;i++){if(a[i]!=b[i]||!a[i]) return (unsigned char)a[i]-(unsigned char)b[i];} return 0; }
char* strncpy(char* d, const char* s, size_t n) { size_t i; for(i=0;i<n&&s[i];i++) d[i]=s[i]; for(;i<n;i++) d[i]='\0'; return d; }
