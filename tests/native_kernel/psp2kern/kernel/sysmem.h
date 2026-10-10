#ifndef VCO_TEST_KERNEL_SYSMEM_H
#define VCO_TEST_KERNEL_SYSMEM_H
#include <stddef.h>
int ksceKernelCopyFromUser(void *, const void *, size_t);
int ksceKernelCopyToUser(void *, const void *, size_t);
#endif
