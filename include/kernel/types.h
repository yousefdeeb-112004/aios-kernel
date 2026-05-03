#ifndef _KERNEL_TYPES_H
#define _KERNEL_TYPES_H
typedef unsigned char uint8_t; typedef unsigned short uint16_t;
typedef unsigned int uint32_t; typedef unsigned long long uint64_t;
typedef signed char int8_t; typedef signed short int16_t;
typedef signed int int32_t; typedef signed long long int64_t;
typedef uint32_t size_t; typedef int32_t ssize_t; typedef uint32_t uintptr_t;
typedef enum { false = 0, true = 1 } bool;
#define NULL ((void*)0)
#ifdef AI_FEATURES
#define AI_ENABLED 1
#else
#define AI_ENABLED 0
#endif
#endif
