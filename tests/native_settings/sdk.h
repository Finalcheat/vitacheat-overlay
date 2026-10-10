#ifndef VCO_TEST_SETTINGS_SDK_H
#define VCO_TEST_SETTINGS_SDK_H
#include <stdint.h>
#include <stddef.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_CREAT 0x200
#define SCE_O_TRUNC 0x400
int sceIoOpen(const char *, int, int);
int sceIoRead(int, void *, size_t);
int sceIoWrite(int, const void *, size_t);
int sceIoClose(int);
int sceIoSyncByFd(int, int);
int sceIoSync(const char *, int);
int sceIoRemove(const char *);
int sceIoRename(const char *, const char *);
int sceIoMkdir(const char *, int);
int sceKernelCreateMutex(const char *, unsigned, int, void *);
int sceKernelLockMutex(int, int, void *);
int sceKernelTryLockMutex(int, int);
int sceKernelUnlockMutex(int, int);
int sceKernelDeleteMutex(int);
int sceKernelCreateThread(const char *, int (*)(SceSize, void *), int, SceSize, unsigned, int,
                          void *);
int sceKernelStartThread(int, SceSize, void *);
int sceKernelDeleteThread(int);
int sceKernelDelayThread(unsigned);
#endif
