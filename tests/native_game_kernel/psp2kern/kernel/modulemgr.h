#ifndef VCO_TEST_GAME_KERNEL_MODULEMGR_H
#define VCO_TEST_GAME_KERNEL_MODULEMGR_H
#include <psp2kern/kernel/proc_event.h>
#include <stdint.h>
typedef struct {
    SceSize size;
    uint32_t perms;
    void *vaddr;
    SceSize memsz, filesz;
    uint32_t res;
} SceKernelSegmentInfo;
typedef struct {
    SceSize size;
    SceUID modid;
    uint16_t modattr;
    uint8_t modver[2];
    char module_name[28];
    uint32_t unk28;
    void *start_entry, *stop_entry, *exit_entry, *exidx_top, *exidx_btm, *extab_top, *extab_btm;
    void *tlsInit;
    SceSize tlsInitSize, tlsAreaSize;
    char path[256];
    SceKernelSegmentInfo segments[4];
    uint32_t state;
} SceKernelModuleInfo;
#define SCE_KERNEL_MODULE_STATE_READY 2u
#define SCE_KERNEL_MODULE_STATE_STARTED 6u
#define SCE_KERNEL_MODULE_STATE_ENDED 9u
#define SCE_KERNEL_STOP_CANCEL 1u
typedef struct {
    int unused;
} SceKernelLMOption;
typedef SceKernelLMOption SceKernelULMOption;
SceUID ksceKernelLoadModuleForPid(SceUID, const char *, int, SceKernelLMOption *);
int ksceKernelStartModuleForPid(SceUID, SceUID, SceSize, void *, int, SceKernelLMOption *, int *);
int ksceKernelStopUnloadModuleForPid(SceUID, SceUID, SceSize, void *, int, SceKernelULMOption *,
                                     int *);
#endif
