#ifndef VCO_TEST_USER_THREADMGR_H
#define VCO_TEST_USER_THREADMGR_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef int (*SceKernelThreadEntry)(SceSize, void *);
typedef struct {
    SceSize size;
    int32_t stackSize;
    uint64_t runClocks;
} SceKernelThreadInfo;
int sceKernelGetThreadInfo(SceUID, SceKernelThreadInfo *);
int64_t sceKernelGetSystemTimeWide(void);
SceUID sceKernelCreateMutex(const char *, unsigned, int, const void *);
int sceKernelDeleteMutex(SceUID);
int sceKernelLockMutex(SceUID, int, SceUInt *);
int sceKernelTryLockMutex(SceUID, int);
int sceKernelUnlockMutex(SceUID, int);
SceUID sceKernelCreateThread(const char *, SceKernelThreadEntry, int, SceSize, unsigned, int,
                             const void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelDeleteThread(SceUID);
int sceKernelWaitThreadEnd(SceUID, int *, SceUInt *);
int sceKernelDelayThread(SceUInt);
int sceKernelExitDeleteThread(int);
#endif
