#ifndef VCO_TEST_SHELL_BINDING_SDK_H
#define VCO_TEST_SHELL_BINDING_SDK_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
#define SCE_O_RDONLY 1
#define SCE_KERNEL_START_FAILED -1
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_STOP_CANCEL -1
#define SCE_TOUCH_PORT_FRONT 0
#define SCE_TOUCH_PORT_BACK 1
#define SCE_TOUCH_SAMPLING_STATE_START 1
#ifdef __cplusplus
extern "C" {
#endif
uint64_t sceKernelGetSystemTimeWide(void);
int sceKernelDelayThread(unsigned);
int sceKernelExitDeleteThread(int);
SceUID sceKernelCreateThread(const char *, int (*)(SceSize, void *), int, unsigned, int, int,
                             void *);
int sceKernelStartThread(SceUID, unsigned, void *);
int sceKernelDeleteThread(SceUID);
int sceIoOpen(const char *, int, int);
int sceIoClose(SceUID);
int sceTouchSetSamplingState(int, int);
#ifdef __cplusplus
}
namespace paf
{
namespace common
{
struct MainThreadCallList {
    static void Register(void (*)(void *), void *);
};
} // namespace common
} // namespace paf
#endif
#endif
