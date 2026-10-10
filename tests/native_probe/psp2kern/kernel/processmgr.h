#ifndef TEST_PROBE_PROCESSMGR_H
#define TEST_PROBE_PROCESSMGR_H
#include <stddef.h>
#include <stdint.h>
typedef int32_t SceUID;
typedef size_t SceSize;
typedef struct { SceSize size; } SceKernelProcessInfo;
int ksceKernelGetProcessInfo(SceUID pid, SceKernelProcessInfo *info);
int ksceKernelGetProcessStatus(SceUID pid, int *status);
#endif
