#ifndef VCO_TEST_GAME_USER_SYSMEM_H
#define VCO_TEST_GAME_USER_SYSMEM_H
#include <psp2/kernel/threadmgr.h>
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0C20D060u
int sceKernelAllocMemBlock(const char *, unsigned int, SceSize, void *);
int sceKernelGetMemBlockBase(int, void **);
int sceKernelFreeMemBlock(int);
#endif
