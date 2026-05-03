#ifndef _LIB_STRING_H
#define _LIB_STRING_H
#include <kernel/types.h>
void* memset(void* dest, int val, size_t count); void* memcpy(void* dest, const void* src, size_t count);
int memcmp(const void* a, const void* b, size_t count); size_t strlen(const char* str);
int strcmp(const char* a, const char* b); char* strcpy(char* dest, const char* src);
int strncmp(const char* a, const char* b, size_t n);
char* strncpy(char* dest, const char* src, size_t n);
#endif
