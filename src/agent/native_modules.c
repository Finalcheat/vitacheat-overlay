#include "native_modules.h"
#include "native_service.h"
#include <taihen.h>

static tai_hook_ref_t references[8];
static int32_t hooks[8];
static uint32_t phase;
static int failure;
#define MODULE_BUSY ((int32_t)UINT32_C(0x8002D01D))

/* These public syscall bodies consume at most r0-r3, including the packed
 * fourth start/stop parameter. Forward the four ARM argument words unchanged;
 * never dereference user parameters or turn them into kernel SDK options. The
 * SDK's high-level six-argument LibKernel wrappers are not these exports. */
#define MODULE_HOOK(name, index)                                                                   \
    static int name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)                            \
    {                                                                                              \
        VcoModuleCall call;                                                                        \
        if (vco_agent_kernel_module_begin(&call) != VCO_AGENT_OK) {                                \
            vco_agent_kernel_module_trace(index, MODULE_BUSY, &call);                              \
            return MODULE_BUSY;                                                                    \
        }                                                                                          \
        int result = TAI_CONTINUE(int, references[index], a, b, c, d);                             \
        vco_agent_kernel_module_trace(index, result, &call);                                       \
        (void)vco_agent_kernel_module_end(&call);                                                  \
        return result;                                                                             \
    }
MODULE_HOOK(load, 0)
MODULE_HOOK(load_start, 1)
MODULE_HOOK(start, 2)
MODULE_HOOK(stop, 3)
MODULE_HOOK(unload, 4)
MODULE_HOOK(stop_unload, 5)
MODULE_HOOK(open, 6)
MODULE_HOOK(close, 7)

int vco_agent_modules_start(void)
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&phase, &expected, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return expected == 2 ? 0 : expected == 3 ? failure : -1;
    static const uint32_t nids[] = {0xB4C5EF9E, 0x60647592, 0x72CD301F, 0x086867A8,
                                    0x8E4A7716, 0x86EAEA0A, 0x9D674F45, 0x849E78BE};
    static const void *const callbacks[] = {load,   load_start,  start, stop,
                                            unload, stop_unload, open,  close};
    int error = 0;
    for (unsigned i = 0; !error && i < 8; ++i) {
        hooks[i] = taiHookFunctionExportForKernel(KERNEL_PID, &references[i], "SceKernelModulemgr",
                                                  0xEAED1616, nids[i], callbacks[i]);
        if (hooks[i] < 0)
            error = hooks[i];
    }
    failure = error;
    if (error)
        vco_agent_kernel_loader_failed(error);
    __atomic_store_n(&phase, error ? 3 : 2, __ATOMIC_RELEASE);
    /* Even partial installations retain their references/code until reboot.
     * FAILED service disables execution, and hooks pass unrelated calls on. */
    return error;
}
