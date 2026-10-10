#ifndef VCO_TEST_HARNESS_MODULEMGR_H
#define VCO_TEST_HARNESS_MODULEMGR_H
#include <stdint.h>
typedef uint32_t SceSize;
typedef int32_t SceUID;
SceUID sceKernelGetModuleIdByAddr(void *);
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_START_FAILED 2
#define SCE_KERNEL_STOP_SUCCESS 0
#endif
