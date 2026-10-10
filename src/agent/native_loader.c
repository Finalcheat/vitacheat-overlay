#include "native_loader.h"
#include "native_service.h"
#include "native_access.h"
#include "native_modules.h"
#include "../storage_paths.h"
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
#include <string.h>
static tai_hook_ref_t reference;
static tai_hook_ref_t file_reference;
static int32_t file_hook = -1;
static uint32_t loading_pid;
static uint32_t fallback_busy;
static int32_t hook = -1;
static uint32_t phase;
static int failure;
int module_get_export_func(SceUID, const char *, uint32_t, uint32_t, uintptr_t *);
static SceUID (*load_module)(SceUID, const char *, int, SceKernelLMOption *);
static int (*start_module)(SceUID, SceUID, SceSize, void *, int, SceKernelLMOption *, int *);
static int resolve_stages(void)
{
    /* These kernel exports changed NIDs after 3.60. Resolve the same SDK
     * entry points by firmware pair, as the existing module-access adapter. */
    const uint32_t exports[2][3] = {{0xC445FA63, 0xFA21D8CB, 0x6DF745D5},
                                    {0x92C9FFC2, 0x4E85022D, 0x3FE47DDF}};
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t load = 0, start = 0;
        if (module_get_export_func(KERNEL_PID, "SceKernelModulemgr", exports[i][0], exports[i][1],
                                   &load) >= 0 &&
            load &&
            module_get_export_func(KERNEL_PID, "SceKernelModulemgr", exports[i][0], exports[i][2],
                                   &start) >= 0 &&
            start) {
            load_module = (SceUID (*)(SceUID, const char *, int, SceKernelLMOption *))load;
            start_module =
                (int (*)(SceUID, SceUID, SceSize, void *, int, SceKernelLMOption *, int *))start;
            return 0;
        }
    }
    return -1;
}
static int load_for_game(SceUID pid, VcoAgentBinding *binding, int *status)
{
    /* Keep an explicit kernel caller scope while SDK calls consume our kernel
     * path/binding buffers. Only this thread changes; module_start runs on its
     * own game user thread. The ordinary LoadModuleForPid also scopes itself. */
    int permission = ksceKernelSetPermission(0x80);
    vco_agent_kernel_loader_scope(pid, 0, permission);
    if (permission < 0)
        return permission;
    SceUID previous_pid = ksceKernelSetProcessIdToTLS(KERNEL_PID);
    vco_agent_kernel_loader_scope(pid, 1, previous_pid);
    int module = previous_pid;
    if (previous_pid >= 0) {
        /* Separate SDK stages: a combined return cannot distinguish file/
         * import loading from entry-thread creation and argument copying. */
        module = load_module(pid, VCO_GAME_PATH, 0, NULL);
        vco_agent_kernel_loader_stage(pid, 0, module, 0);
        if (module >= 0) {
            int started = start_module(pid, module, sizeof(*binding), binding, 0, NULL, status);
            vco_agent_kernel_loader_stage(pid, 1, started, (uint32_t)*status);
            if (started < 0 || *status != 0) {
                /* Respect module_stop's CANCEL if an entry thread survived a
                 * failed start. Never force-unload possibly live game code.
                 * A failed cleanup stays process-owned until game exit. */
                int stopped = -1;
                int cleanup =
                    ksceKernelStopUnloadModuleForPid(pid, module, 0, NULL, 0, NULL, &stopped);
                vco_agent_kernel_loader_stage(pid, 2, cleanup, (uint32_t)stopped);
                module = started < 0 ? started : -1;
            }
        }
        int restored = ksceKernelSetProcessIdToTLS(previous_pid);
        vco_agent_kernel_loader_scope(pid, 2, restored);
        if (restored < 0)
            module = restored;
    }
    int restored = ksceKernelSetPermission(permission);
    vco_agent_kernel_loader_scope(pid, 3, restored);
    return restored < 0 ? restored : module;
}
static int open_for_pid(SceUID pid, const char *path, int flags, SceMode mode)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int result = TAI_CONTINUE(int, file_reference, pid, path, flags, mode);
    /* Modulemgr opens as the target game. Limit the kernel fallback to our
     * exact read-only plugin file and the PID of our synchronous load. */
    if (pid > 0 && __atomic_load_n(&loading_pid, __ATOMIC_ACQUIRE) == (uint32_t)pid && path &&
        (flags & SCE_O_RDWR) == SCE_O_RDONLY &&
        !(flags & (SCE_O_APPEND | SCE_O_CREAT | SCE_O_TRUNC)) &&
        !strncmp(path, VCO_GAME_PATH, sizeof(VCO_GAME_PATH))) {
        vco_agent_kernel_file_trace(pid, result, false);
        if (result < 0) {
            uint32_t empty = 0;
            if (__atomic_compare_exchange_n(&fallback_busy, &empty, 1, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                result = ksceIoOpen(VCO_GAME_PATH, flags, mode);
                __atomic_store_n(&fallback_busy, 0, __ATOMIC_RELEASE);
                vco_agent_kernel_file_trace(pid, result, true);
            }
        }
    }
    EXIT_SYSCALL(state);
    return result;
}
static VcoAgentStatus begin_for_game(SceUID pid, const char title[10], VcoAgentBinding *binding)
{
    VcoAgentStatus status;
    unsigned attempts = 0;
    do {
        ++attempts;
        status = vco_agent_kernel_begin(pid, title, binding);
        if (status != VCO_AGENT_BUSY || attempts == 8u)
            break;
        /* BUSY is returned before a binding is created. Retry only that
         * admission, outside the gate; module load/start still runs once. */
        uint32_t state;
        ENTER_SYSCALL(state);
        int delayed = ksceKernelDelayThread(1000u);
        EXIT_SYSCALL(state);
        if (delayed < 0)
            break;
    } while (true);
    vco_agent_kernel_binding_result(pid, status, attempts);
    return status;
}
static int preloaded(SceUID pid)
{
    if (!reference)
        return -1;
    int original = TAI_CONTINUE(int, reference, pid);
    if (original < 0)
        return original;
    char title[32];
    if (vco_agent_kernel_title(pid, title) < 0 ||
        (memcmp(title, "PC", 2) && memcmp(title, "VCOCH0001", 10)))
        return original;
    VcoAgentBinding binding;
    VcoAgentStatus accepted = begin_for_game(pid, title, &binding);
    if (accepted != VCO_AGENT_OK)
        return original;
    /* begin released the broker gate. The game's module_start may immediately
     * register/call the broker; never hold its gate across module entry. */
    int status = -1;
    uint32_t empty = 0;
    int module = -1;
    if (__atomic_compare_exchange_n(&loading_pid, &empty, (uint32_t)pid, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        /* A preload hook can inherit a user syscall context. Match taiHEN's
         * kernel load helper before passing kernel path/argument pointers;
         * restore that context even when the SDK load returns an error. */
        uint32_t state;
        ENTER_SYSCALL(state);
        vco_agent_kernel_loader_context(pid, state);
        module = load_for_game(pid, &binding, &status);
        EXIT_SYSCALL(state);
        __atomic_store_n(&loading_pid, 0, __ATOMIC_RELEASE);
    }
    vco_agent_kernel_load_result(&binding, module, status);
    return original;
}
int vco_agent_loader_start(void)
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&phase, &expected, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return expected == 2 ? 0 : expected == 3 ? failure : -1;
    int result = vco_agent_kernel_start();
    if (!result) {
        result = vco_agent_access_start();
        if (result < 0)
            vco_agent_kernel_loader_failed(result);
    }
    if (!result) {
        result = resolve_stages();
        if (result < 0)
            vco_agent_kernel_loader_failed(result);
    }
    if (!result) {
        file_hook = taiHookFunctionExportForKernel(KERNEL_PID, &file_reference, "SceIofilemgr",
                                                   0x40FD29C7, 0xC3D34965, open_for_pid);
        result = file_hook < 0 ? file_hook : 0;
        if (result < 0)
            vco_agent_kernel_loader_failed(result);
    }
    if (!result)
        result = vco_agent_modules_start();
    if (!result) {
        hook = taiHookFunctionExportForKernel(KERNEL_PID, &reference, "SceKernelModulemgr",
                                              0xC445FA63, 0x432DCC7A, preloaded);
        if (hook < 0)
            hook = taiHookFunctionExportForKernel(KERNEL_PID, &reference, "SceKernelModulemgr",
                                                  0x92C9FFC2, 0x998C7AE9, preloaded);
        result = hook < 0 ? hook : 0;
        if (result < 0)
            vco_agent_kernel_loader_failed(result);
    }
    failure = result;
    __atomic_store_n(&phase, result ? 3 : 2, __ATOMIC_RELEASE);
    /* Hook, callback, event handler and broker stay resident until reboot. */
    return result;
}
