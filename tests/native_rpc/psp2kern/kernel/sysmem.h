#include <stddef.h>
int ksceKernelCopyFromUser(void *out, const void *in, size_t size);
int ksceKernelCopyToUser(void *out, const void *in, size_t size);
