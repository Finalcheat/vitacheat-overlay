#include <stdint.h>
#define SCE_O_WRONLY 1
#define SCE_O_CREAT 2
#define SCE_O_TRUNC 4
#define SCE_O_APPEND 8
#define SCE_SEEK_END 2
int sceIoOpen(const char *, int, int);
int sceIoWrite(int, const void *, unsigned);
int sceIoClose(int);
int64_t sceIoLseek(int, int64_t, int);
