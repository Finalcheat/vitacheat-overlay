#ifndef VCO_TEST_DIAGNOSTICS_KERNEL_FCNTL_H
#define VCO_TEST_DIAGNOSTICS_KERNEL_FCNTL_H
#include <psp2/io/fcntl.h>
int ksceIoOpen(const char *, int, int);
int ksceIoWrite(int, const void *, unsigned);
int ksceIoClose(int);
int64_t ksceIoLseek(int, int64_t, int);
#endif
