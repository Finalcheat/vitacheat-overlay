#ifndef TEST_NATIVE_WORKER_THREADMGR_H
#define TEST_NATIVE_WORKER_THREADMGR_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef int64_t SceInt64;
typedef int (*SceKernelThreadEntry)(SceSize, void *);
typedef struct {
    uint32_t size;
} SceKernelThreadOptParam;
SceUID ksceKernelCreateThread(const char *, SceKernelThreadEntry, int, SceSize, SceUInt, int,
                              const SceKernelThreadOptParam *);
int ksceKernelStartThread(SceUID, SceSize, void *);
int ksceKernelWaitThreadEnd(SceUID, int *, SceUInt *);
int ksceKernelDeleteThread(SceUID);
int ksceKernelDelayThread(SceUInt);
SceInt64 ksceKernelGetSystemTimeWide(void);
#endif
