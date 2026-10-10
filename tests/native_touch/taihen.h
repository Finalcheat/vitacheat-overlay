#ifndef TEST_TOUCH_TAI_H
#define TEST_TOUCH_TAI_H
#include <psp2/touch.h>
#include <stdint.h>
typedef uintptr_t tai_hook_ref_t;
#define KERNEL_PID 0x10005
int taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module,
                                   uint32_t library, uint32_t nid, const void *function);
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref);
int fake_touch_continue(tai_hook_ref_t ref, unsigned port, SceTouchData *data, unsigned count, ...);
#define TAI_CONTINUE(type, ref, ...) fake_touch_continue(ref, __VA_ARGS__)
#endif
