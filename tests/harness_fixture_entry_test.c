#include "../tools/device-harness/fixture.h"
#include <psp2/kernel/modulemgr.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int module_start(SceSize, void *);
int module_stop(SceSize, void *);
/* Only the four-byte Thumb function is substituted here. The shared production
 * entry C code is linked unchanged; real ARM instruction bytes are checked at
 * build/package validation rather than claimed as a host execution result. */
int fixture_target(void)
{
    return VCO_FIXTURE_VARIANT ? 11 : 7;
}
static int uid_failure;
SceUID sceKernelGetModuleIdByAddr(void *address)
{
    assert(address == (void *)(uintptr_t)fixture_target);
    return uid_failure ? -1 : 100 + VCO_FIXTURE_VARIANT;
}
int main(void)
{
    VcoFixtureReply reply, unchanged;
    memset(&reply, 0xa5, sizeof(reply));
    unchanged = reply;
    VcoFixtureStart start = {VCO_FIXTURE_ABI, sizeof(start), 19, VCO_FIXTURE_VARIANT, &reply};
    assert(module_start(sizeof(start) - 1, &start) == SCE_KERNEL_START_FAILED);
    assert(module_start(sizeof(start), NULL) == SCE_KERNEL_START_FAILED);
    assert(module_start(sizeof(start), (char *)&start + 1) == SCE_KERNEL_START_FAILED);
    assert(!memcmp(&reply, &unchanged, sizeof(reply)));
    for (unsigned i = 0; i < 6; ++i) {
        VcoFixtureStart bad = start;
        if (i == 0)
            bad.abi++;
        if (i == 1)
            bad.size--;
        if (i == 2)
            bad.generation = 0;
        if (i == 3)
            bad.variant ^= 1;
        if (i == 4)
            bad.reply = NULL;
        if (i == 5)
            bad.reply = (VcoFixtureReply *)((char *)&reply + 1);
        assert(module_start(sizeof(bad), &bad) == SCE_KERNEL_START_FAILED);
        assert(!memcmp(&reply, &unchanged, sizeof(reply)));
    }
    uid_failure = 1;
    assert(module_start(sizeof(start), &start) == SCE_KERNEL_START_FAILED);
    assert(!memcmp(&reply, &unchanged, sizeof(reply)));
    uid_failure = 0;
    VcoFixtureStart copied = start;
    assert(module_start(sizeof(copied), &copied) == SCE_KERNEL_START_SUCCESS);
    assert(!memcmp(&start, &copied, sizeof(start)));
    assert(reply.abi == VCO_FIXTURE_ABI && reply.size == sizeof(reply) && reply.generation == 19);
    assert(reply.variant == VCO_FIXTURE_VARIANT && reply.data_address);
    assert(reply.module_uid == 100 + VCO_FIXTURE_VARIANT);
    assert(reply.function_address == (uint32_t)(uintptr_t)fixture_target);
    assert(reply.data_initial == 5 + VCO_FIXTURE_VARIANT);
    assert(reply.function_initial == (VCO_FIXTURE_VARIANT ? 11 : 7));
    VcoFixtureReply completed = reply;
    assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
    assert(!memcmp(&reply, &completed, sizeof(reply)));
    puts("fixture native entry contract passed");
    return 0;
}
