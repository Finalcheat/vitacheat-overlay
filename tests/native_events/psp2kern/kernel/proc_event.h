#ifndef TEST_NATIVE_PROC_EVENT_H
#define TEST_NATIVE_PROC_EVENT_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef struct {
    SceSize size;
    int unk_0x04, unk_0x08, unk_0x0C;
} SceProcEventInvokeParam1;
typedef struct {
    SceSize size;
    SceUID pid;
    int unk_0x08, unk_0x0C, unk_0x10;
} SceProcEventInvokeParam2;
typedef struct {
    SceSize size;
    int (*create)(SceUID, SceProcEventInvokeParam2 *, int);
    int (*exit)(SceUID, SceProcEventInvokeParam1 *, int);
    int (*kill)(SceUID, SceProcEventInvokeParam1 *, int);
    int (*stop)(SceUID, int, SceProcEventInvokeParam1 *, int);
    int (*start)(SceUID, int, SceProcEventInvokeParam1 *, int);
    int (*switch_process)(int, int, SceProcEventInvokeParam2 *, int);
} SceProcEventHandler;
SceUID ksceKernelRegisterProcEventHandler(const char *, const SceProcEventHandler *, int);
int ksceKernelUnregisterProcEventHandler(SceUID);
#endif
