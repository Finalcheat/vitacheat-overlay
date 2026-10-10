#include "modulemgr.h"
int sceKernelDelayThread(unsigned);
int sceKernelExitDeleteThread(int);
int sceKernelCreateThread(const char *, int (*)(SceSize, void *), int, unsigned, unsigned, unsigned, void *);
int sceKernelStartThread(int, unsigned, void *);
int sceKernelDeleteThread(int);
