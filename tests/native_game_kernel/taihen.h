#ifndef VCO_TEST_GAME_KERNEL_TAIHEN_H
#define VCO_TEST_GAME_KERNEL_TAIHEN_H
#include <psp2kern/kernel/sysmem.h>
#include <stdint.h>
#define KERNEL_PID 0x10005
typedef uintptr_t tai_hook_ref_t;
int taiHookFunctionExportForKernel(SceUID, tai_hook_ref_t *, const char *, uint32_t, uint32_t,
                                   const void *);
int fake_game_continue(tai_hook_ref_t, ...);
#define TAI_CONTINUE(type, reference, ...) fake_game_continue(reference, __VA_ARGS__)
#endif
