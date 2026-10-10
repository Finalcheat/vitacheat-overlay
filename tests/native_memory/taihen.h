#define KERNEL_PID 0x10005

#include <stddef.h>
#include <stdint.h>
int32_t taiInjectAbsForKernel(int32_t pid, void *dest, const void *src, size_t size);
int taiInjectReleaseForKernel(int32_t handle);
