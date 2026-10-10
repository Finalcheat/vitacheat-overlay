#include "fixture.h"
#include <psp2/kernel/modulemgr.h>
#ifndef VCO_FIXTURE_VARIANT
#error "Set VCO_FIXTURE_VARIANT to 0 or 1"
#endif
extern int fixture_target(void);
static volatile uint32_t value;
typedef char ReplySizeCheck[sizeof(VcoFixtureReply) == 36 ? 1 : -1];
#if UINTPTR_MAX == UINT32_MAX
typedef char StartSizeCheck[sizeof(VcoFixtureStart) == 20 ? 1 : -1];
#endif
int module_start(SceSize length, void *argument)
{
    if (length != sizeof(VcoFixtureStart) || !argument || ((uintptr_t)argument & 3u))
        return SCE_KERNEL_START_FAILED;
    const VcoFixtureStart *start = argument;
    if (start->abi != VCO_FIXTURE_ABI || start->size != sizeof(*start) || !start->generation ||
        start->variant != VCO_FIXTURE_VARIANT || !start->reply || ((uintptr_t)start->reply & 3u))
        return SCE_KERNEL_START_FAILED;
    SceUID uid = sceKernelGetModuleIdByAddr((void *)(uintptr_t)fixture_target);
    if (uid <= 0)
        return SCE_KERNEL_START_FAILED;
    value = 5u + VCO_FIXTURE_VARIANT;
    /* Same-process explicit output pointer survives Modulemgr's argument copy. */
    VcoFixtureReply *reply = start->reply;
    reply->abi = VCO_FIXTURE_ABI;
    reply->size = sizeof(*reply);
    reply->generation = start->generation;
    reply->variant = VCO_FIXTURE_VARIANT;
    reply->data_address = (uint32_t)(uintptr_t)&value;
    reply->function_address = (uint32_t)(uintptr_t)fixture_target;
    reply->data_initial = 5u + VCO_FIXTURE_VARIANT;
    reply->function_initial = VCO_FIXTURE_VARIANT ? 11 : 7;
    reply->module_uid = uid;
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize length, void *argument)
{
    (void)length;
    (void)argument;
    return SCE_KERNEL_STOP_SUCCESS;
}
