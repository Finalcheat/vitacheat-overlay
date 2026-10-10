#ifndef TEST_NATIVE_THREADMGR_H
#define TEST_NATIVE_THREADMGR_H
#include "sysmem.h"
typedef struct {
    SceSize size;
} SceKernelEventFlagOptParam;
SceUID ksceKernelCreateEventFlag(const char *, int, int, SceKernelEventFlagOptParam *);
int ksceKernelDeleteEventFlag(SceUID);
#endif
