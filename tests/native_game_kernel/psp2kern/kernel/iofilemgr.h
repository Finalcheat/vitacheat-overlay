#ifndef VCO_TEST_GAME_KERNEL_IOFILEMGR_H
#define VCO_TEST_GAME_KERNEL_IOFILEMGR_H
#include <psp2kern/kernel/sysmem.h>
typedef int SceMode;
enum {
    SCE_O_RDONLY = 1,
    SCE_O_WRONLY = 2,
    SCE_O_RDWR = 3,
    SCE_O_APPEND = 0x100,
    SCE_O_CREAT = 0x200,
    SCE_O_TRUNC = 0x400
};
int ksceIoOpen(const char *, int, SceMode);
#endif
