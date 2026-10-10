#ifndef TEST_SERVICE_SYSMEM_H
#define TEST_SERVICE_SYSMEM_H
#include <stddef.h>
#include <stdint.h>
typedef int32_t SceUID;
typedef struct {
    int tag;
} SceClass;
typedef struct {
    int tag;
} SceObjectBase;
#define SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW 1u
int ksceKernelAllocMemBlock(const char *, unsigned, size_t, void *);
int ksceKernelGetMemBlockBase(int, void **);
int ksceKernelFreeMemBlock(int);
int ksceKernelCopyFromUser(void *, const void *, size_t);
int ksceKernelCopyToUser(void *, const void *, size_t);
int ksceGUIDReferObjectWithClass(SceUID, SceClass *, SceObjectBase **);
int ksceGUIDReleaseObject(SceUID);
#endif
