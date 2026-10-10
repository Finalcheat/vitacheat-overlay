#ifndef VCO_TEST_USER_SYSMEM_H
#define VCO_TEST_USER_SYSMEM_H
#include "threadmgr.h"
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 1
typedef struct {
    int size, size_user, size_cdram, size_phycont;
} SceKernelFreeMemorySizeInfo;
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *);
SceUID sceKernelAllocMemBlock(const char *, int, SceSize, const void *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
#endif
