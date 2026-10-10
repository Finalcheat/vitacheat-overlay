#ifndef VCO_TEST_GAME_KERNEL_PROC_EVENT_H
#define VCO_TEST_GAME_KERNEL_PROC_EVENT_H
#include <psp2kern/kernel/sysmem.h>
typedef uint32_t SceSize;
typedef struct {
    SceSize size;
    int a, b, c;
} SceProcEventInvokeParam1;
typedef struct {
    SceSize size;
    SceUID pid;
    int a, b, c;
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
#endif
