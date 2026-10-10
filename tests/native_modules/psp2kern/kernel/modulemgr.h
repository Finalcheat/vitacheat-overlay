#ifndef TEST_NATIVE_MODULEMGR_H
#define TEST_NATIVE_MODULEMGR_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
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
#endif
