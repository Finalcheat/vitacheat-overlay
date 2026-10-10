#include <assert.h>
#include <string.h>
#include <psp2kern/kernel/processmgr.h>
#include <taihen.h>
#include "platform/game_probe.h"
void vco_probe_init(void);
void vco_probe_sample(int32_t pid, VcoGameProbe *out);
static int firmware[2], calls[2], empty_export;
static int module_list(SceUID pid, int flags, int type, SceUID *modules, SceSize *count);
static SceUID main_thread(SceUID pid) { assert(pid == 42); return 123; }
int module_get_export_func(SceUID pid, const char *module, uint32_t library,
                           uint32_t function, uintptr_t *address)
{
    assert(pid == KERNEL_PID);
    int kind = !strcmp(module, "SceKernelModulemgr");
    assert(kind || !strcmp(module, "SceProcessmgr"));
    int candidate = calls[kind]++;
    assert(candidate < 2);
    const uint32_t libraries[2][2] = {{0x7A69DE86, 0xEB1F8EF7}, {0xC445FA63, 0x92C9FFC2}};
    const uint32_t functions[2][2] = {{0x95F9ED94, 0xD20553C6}, {0x97CF7B4E, 0xB72C75A4}};
    assert(library == libraries[kind][candidate] && function == functions[kind][candidate]);
    *address = 0;
    if (candidate != firmware[kind]) return -1;
    if (!empty_export) *address = kind ? (uintptr_t)module_list : (uintptr_t)main_thread;
    return 0;
}

int ksceKernelGetProcessInfo(SceUID pid, SceKernelProcessInfo *info)
{
    assert(pid == 42 && info->size == sizeof(*info)); return 0;
}
int ksceKernelGetProcessStatus(SceUID pid, int *status)
{
    assert(pid == 42); *status = 3; return 0;
}
static int module_list(SceUID pid, int flags, int type, SceUID *modules, SceSize *count)
{
    assert(pid == 42 && flags == 0xff && type == 1 && modules && *count == 32);
    *count = 2; return 0;
}
int main(void)
{
    /* Includes independent absence: diagnostics must degrade field by field. */
    for (firmware[0] = 0; firmware[0] < 3; ++firmware[0]) {
        for (firmware[1] = 0; firmware[1] < 3; ++firmware[1]) {
            calls[0] = calls[1] = 0; vco_probe_init();
            for (int i = 0; i < 2; ++i) assert(calls[i] == (firmware[i] ? 2 : 1));
            VcoGameProbe probe;
            vco_probe_sample(42, &probe);
            assert(probe.abi == VCO_PROBE_ABI && probe.size == sizeof(probe));
            assert(probe.main_thread == (firmware[0] < 2 ? 123 : -1));
            assert(!probe.info_result && !probe.status_result);
            assert(probe.module_count == (firmware[1] < 2 ? 2u : 0u));
            assert(probe.modules_result == (firmware[1] < 2 ? 0 : -1));
            assert(!probe.verified_capabilities);
        }
    }
    firmware[0] = firmware[1] = 0; empty_export = 1; calls[0] = calls[1] = 0;
    vco_probe_init();
    VcoGameProbe probe; vco_probe_sample(42, &probe);
    assert(calls[0] == 2 && calls[1] == 2 && probe.main_thread == -1 && probe.modules_result == -1);
    return 0;
}
