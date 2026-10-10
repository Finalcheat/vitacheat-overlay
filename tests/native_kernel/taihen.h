#ifndef VCO_TEST_KERNEL_TAIHEN_H
#define VCO_TEST_KERNEL_TAIHEN_H
#include <psp2/touch.h>
#include <stdint.h>
typedef uintptr_t tai_hook_ref_t;
#define KERNEL_PID 0x10005
int taiHookFunctionExportForKernel(SceUID, tai_hook_ref_t *, const char *, uint32_t, uint32_t,
                                   const void *);
int taiHookReleaseForKernel(SceUID, tai_hook_ref_t);
int fake_kernel_continue(tai_hook_ref_t, unsigned, void *, unsigned, ...);
#define TAI_CONTINUE(type, ref, ...) fake_kernel_continue(ref, __VA_ARGS__)
#endif
