#ifndef TEST_SERVICE_TAIHEN_H
#define TEST_SERVICE_TAIHEN_H
#include <stddef.h>
#include <stdint.h>
#define KERNEL_PID 0x10005
typedef struct {
    size_t size;
} tai_module_info_t;
int taiGetModuleInfoForKernel(int32_t, const char *, tai_module_info_t *);
#endif
