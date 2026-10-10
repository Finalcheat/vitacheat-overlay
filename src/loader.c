#include "version.h"
#include "storage_paths.h"
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include "taihen.h"
#define VCO_LOG_PATH VCO_LOADER_LOG
#include "diagnostics.h"
#include "paf_checks.h"

/* No PAF or private bridge imports: a failed UI link must remain observable. */
static int dependencies_ready(uint32_t *missing)
{
    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfo("ScePaf", &info) < 0) {
        *missing = 0;
        return 0;
    }
    for (unsigned i = 0; i < sizeof(paf_checks) / sizeof(paf_checks[0]); ++i) {
        uintptr_t address = 0;
        if (taiGetModuleExportFunc("ScePaf", paf_checks[i].library, paf_checks[i].function,
                                   &address) < 0 ||
            !address) {
            *missing = paf_checks[i].function;
            return 0;
        }
    }
    return 1;
}

static int boot(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    uint32_t missing = 0;
    unsigned attempt;
    for (attempt = 0; attempt < 120; ++attempt) {
        if (dependencies_ready(&missing))
            break;
        if (sceKernelDelayThread(250000) < 0)
            return sceKernelExitDeleteThread(-1);
    }
    if (attempt == 120) {
        vco_log("paf_readiness_timeout", missing, 0);
        return sceKernelExitDeleteThread(-1);
    }
    vco_log("paf_ready", attempt, 0);
    SceUID module = sceKernelLoadModule(VCO_UI_PATH, 0, NULL);
    vco_log("ui_load_result", (unsigned)module, 0);
    if (module >= 0) {
        int status = -1;
        int result = sceKernelStartModule(module, 0, NULL, 0, NULL, &status);
        vco_log("ui_start_result", (unsigned)result, 0);
        vco_log("ui_start_status", (unsigned)status, 0);
        /* Never unload a started module: it may own callbacks and a thread. */
    }
    return sceKernelExitDeleteThread(0);
}

int module_start(SceSize args, const void *argp)
{
    (void)args;
    (void)argp;
    vco_log("loader_start_" VCO_VERSION, 0, 1);
    SceUID thread = sceKernelCreateThread("vco_loader", boot, 248, 16384, 0, 0, NULL);
    vco_log("loader_thread", (unsigned)thread, 0);
    if (thread < 0)
        return SCE_KERNEL_START_FAILED;
    int result = sceKernelStartThread(thread, 0, NULL);
    vco_log("loader_thread_start", (unsigned)result, 0);
    if (result < 0) {
        sceKernelDeleteThread(thread);
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, const void *argp)
{
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_CANCEL;
}
