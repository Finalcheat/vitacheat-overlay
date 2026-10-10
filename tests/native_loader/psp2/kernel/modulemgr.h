#ifndef TEST_LOADER_MODULE_H
#define TEST_LOADER_MODULE_H
#include <stddef.h>
#include <stdint.h>
typedef int SceUID;
typedef unsigned SceSize;
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_START_FAILED (-1)
#define SCE_KERNEL_STOP_CANCEL 1
int sceKernelLoadModule(const char *, int, void *);
int sceKernelStartModule(int, unsigned, void *, int, void *, int *);
#endif
