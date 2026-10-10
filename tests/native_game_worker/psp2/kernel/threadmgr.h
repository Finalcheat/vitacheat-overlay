#ifndef VCO_FAKE_GAME_THREADMGR_H
#define VCO_FAKE_GAME_THREADMGR_H
#include <stdint.h>
#include <stddef.h>
typedef size_t SceSize;
typedef unsigned int SceUInt;
typedef int64_t SceInt64;
typedef int (*SceKernelThreadEntry)(SceSize, void *);
int sceKernelCreateThread(const char *, SceKernelThreadEntry, int, unsigned int, unsigned int, int,
                          const void *);
int sceKernelStartThread(int, SceSize, void *);
int sceKernelWaitThreadEnd(int, int *, SceUInt *);
int sceKernelDeleteThread(int);
int sceKernelDelayThread(SceUInt);
SceInt64 sceKernelGetSystemTimeWide(void);
#endif
