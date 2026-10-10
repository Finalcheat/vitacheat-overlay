#include "game_probe.h"
#include <psp2kern/kernel/processmgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <taihen.h>
#include "diagnostics.h"

/* taiHEN module-utils ABI; lookup avoids a hard dependency on firmware-specific
 * SceProcessmgrForKernel NIDs. These are the SDK DB mappings for 3.60 and 3.63+. */
int module_get_export_func(SceUID pid, const char *module, uint32_t library,
                           uint32_t function, uintptr_t *address);
typedef SceUID (*MainThreadFn)(SceUID);
typedef int (*ModuleListFn)(SceUID, int, int, SceUID *, SceSize *);
static MainThreadFn get_main_thread;
static ModuleListFn get_module_list;
static uintptr_t resolve(const char *module, const char *event, const uint32_t libraries[2],
                         const uint32_t functions[2])
{
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t address = 0;
        int result = module_get_export_func(KERNEL_PID, module, libraries[i], functions[i], &address);
        vco_log(event, (unsigned)result, 0);
        if (result >= 0 && address) {
            vco_log("probe_resolved_nid", functions[i], 0);
            return address;
        }
    }
    return 0;
}
void vco_probe_init(void)
{
    const uint32_t process_libraries[] = {0x7A69DE86, 0xEB1F8EF7};
    const uint32_t process_functions[] = {0x95F9ED94, 0xD20553C6};
    const uint32_t module_libraries[] = {0xC445FA63, 0x92C9FFC2};
    const uint32_t module_functions[] = {0x97CF7B4E, 0xB72C75A4};
    get_main_thread = (MainThreadFn)resolve("SceProcessmgr", "probe_main_thread_lookup",
                                           process_libraries, process_functions);
    get_module_list = (ModuleListFn)resolve("SceKernelModulemgr", "probe_module_list_lookup",
                                           module_libraries, module_functions);
}
/* Only called with a kernel-owned output object. Status bit meanings, module
 * order and thread UID lifetime are recorded for the combined device test. */
void vco_probe_sample(int32_t pid, VcoGameProbe *out)
{
    *out = (VcoGameProbe){0};
    out->abi = VCO_PROBE_ABI;
    out->size = sizeof(*out);
    out->pid = pid;
    SceKernelProcessInfo info = {0};
    info.size = sizeof(info);
    out->info_result = ksceKernelGetProcessInfo(pid, &info);
    out->status_result = ksceKernelGetProcessStatus(pid, &out->process_status);
    out->main_thread = get_main_thread ? get_main_thread(pid) : -1;
    SceUID modules[32];
    SceSize count = 32;
    out->modules_result = get_module_list ? get_module_list(pid, 0xff, 1, modules, &count) : -1;
    if (out->modules_result >= 0)
        out->module_count = count;
    /* No foreground authority or lifecycle guarantee has been demonstrated. */
    out->verified_capabilities = 0;
}
