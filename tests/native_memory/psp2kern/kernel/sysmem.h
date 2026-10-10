#ifndef TEST_NATIVE_SYSMEM_H
#define TEST_NATIVE_SYSMEM_H
#include <stddef.h>
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef struct {
    int tag;
} SceClass;
typedef struct {
    int tag;
} SceObjectBase;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R 0x0C20D040
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RX 0x0C20D050
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW 0x0C20D060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW 0x0C208060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW 0x0C50D060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW 0x0C80D060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW 0x0D808060
int ksceGUIDReferObjectWithClass(SceUID, SceClass *, SceObjectBase **);
int ksceGUIDReferObject(SceUID, SceObjectBase **);
int ksceGUIDReleaseObject(SceUID);
int ksceKernelLockRangeProc(SceUID, void *, SceSize);
int ksceKernelUnlockRangeProc(SceUID, void *, SceSize);
SceUID ksceKernelFindProcMemBlockByAddr(SceUID, const void *, SceSize);
int ksceKernelGetMemBlockType(SceUID, unsigned *);
int ksceKernelCopyFromUserProc(SceUID, void *, const void *, SceSize);
int ksceKernelCopyToUserProc(SceUID, void *, const void *, SceSize);
#endif
