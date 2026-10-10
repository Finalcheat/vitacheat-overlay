#ifndef VCO_TEST_USER_FCNTL_H
#define VCO_TEST_USER_FCNTL_H
#include <stddef.h>
#include <stdint.h>
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_CREAT 4
#define SCE_O_TRUNC 8
#define SCE_O_APPEND 16
#define SCE_SEEK_END 2
int sceIoOpen(const char *, int, int);
int sceIoRead(int, void *, size_t);
int sceIoWrite(int, const void *, size_t);
int sceIoClose(int);
int64_t sceIoLseek(int, int64_t, int);
#endif
