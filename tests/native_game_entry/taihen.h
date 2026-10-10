#ifndef VCO_TEST_GAME_ENTRY_TAIHEN_H
#define VCO_TEST_GAME_ENTRY_TAIHEN_H
#include "../native_game_memory/taihen.h"
typedef uintptr_t tai_hook_ref_t;
#define KERNEL_PID 0x10005
int taiHookFunctionExportForKernel(int32_t, tai_hook_ref_t *, const char *, uint32_t, uint32_t,
                                   const void *);
int entry_module_continue(tai_hook_ref_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#define TAI_CONTINUE(type, reference, ...) entry_module_continue(reference, __VA_ARGS__)
#endif
