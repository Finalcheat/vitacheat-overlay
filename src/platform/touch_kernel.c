#include "touch_kernel.h"
#include <psp2/touch.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
/* Driver ABI absent from SDK declarations, but present in its SceTouch NID DB.
 * Layouts are SDK types; signatures cross-checked with ds4vita/reVita declarations. */
int ksceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo *info);
static tai_hook_ref_t refs[4];
static SceUID hooks[4] = {-1, -1, -1, -1};
static uint32_t enabled;
static SceTouchPanelInfo front_panel;
static uint32_t available;
static uint32_t captured_stamp[2], cutoff_deadline[2];
static int filter(unsigned port, SceTouchData *data, int result, unsigned requested, bool before,
                  uint32_t epoch)
{
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return result;
    bool suppress = before || vco_kernel_capturing() || epoch != vco_kernel_capture_epoch();
    if (result <= 0)
        return result;
    uint32_t now = ksceKernelGetSystemTimeLow();
    uint32_t deadline = port < 2 ? __atomic_load_n(&cutoff_deadline[port], __ATOMIC_ACQUIRE) : 0;
    uint32_t cutoff = port < 2 ? __atomic_load_n(&captured_stamp[port], __ATOMIC_ACQUIRE) : 0;
    bool recent = deadline && (int32_t)(deadline - now) > 0;
    if (!suppress && !recent)
        return result;
    if (!data || result > 64 || (unsigned)result > requested)
        return 0;
    for (int i = 0; i < result; ++i) {
        bool old_capture = recent && (int32_t)((uint32_t)data[i].timeStamp - cutoff) <= 0;
        if (!suppress && !old_capture)
            continue;
        data[i].reportNum = 0;
        unsigned char *bytes = (unsigned char *)data[i].report;
        for (unsigned j = 0; j < sizeof(data[i].report); ++j)
            bytes[j] = 0;
    }
    return result;
}
#define TOUCH_HOOK(n)                                                                              \
    static int touch_##n(SceUInt32 port, SceTouchData *data, SceUInt32 count)                      \
    {                                                                                              \
        uint32_t epoch = vco_kernel_capture_epoch();                                               \
        bool before = vco_kernel_capturing();                                                      \
        int result = TAI_CONTINUE(int, refs[n], port, data, count);                                \
        return filter(port, data, result, count, before, epoch);                                   \
    }
#define REGION_HOOK(n)                                                                             \
    static int touch_##n(SceUInt32 port, SceTouchData *data, SceUInt32 count, int region)          \
    {                                                                                              \
        uint32_t epoch = vco_kernel_capture_epoch();                                               \
        bool before = vco_kernel_capturing();                                                      \
        int result = TAI_CONTINUE(int, refs[n], port, data, count, region);                        \
        return filter(port, data, result, count, before, epoch);                                   \
    }
TOUCH_HOOK(0)
TOUCH_HOOK(1)
REGION_HOOK(2)
REGION_HOOK(3)
void vco_touch_kernel_stop(void)
{
    /* No hot unhook: callers may still be inside the original touch function.
     * Keep refs and panel/history storage intact for their safe return. */
    __atomic_store_n(&enabled, 0, __ATOMIC_RELEASE);
}
int vco_touch_kernel_start(void)
{
    if (__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (__atomic_load_n(&hooks[i], __ATOMIC_ACQUIRE) >= 0)
            return -1; /* A stopped/partial resident instance still owns these refs. */
    available = 0;
    for (unsigned i = 0; i < 2; ++i)
        captured_stamp[i] = cutoff_deadline[i] = 0;
    const uint32_t nids[] = {0xBAD1960B, 0x70C8AACE, 0x9B3F7207, 0x9A91F624};
    const void *functions[] = {touch_0, touch_1, touch_2, touch_3};
    const char *module = "SceTouch";
    for (unsigned i = 0; i < 4; ++i) {
        SceUID hook = taiHookFunctionExportForKernel(KERNEL_PID, &refs[i], module, 0xA4A7DAFC,
                                                     nids[i], functions[i]);
        __atomic_store_n(&hooks[i], hook, __ATOMIC_RELEASE);
        if (!i && hooks[i] < 0) {
            module = "SceTouchDummy";
            hook = taiHookFunctionExportForKernel(KERNEL_PID, &refs[i], module, 0xA4A7DAFC,
                                                  nids[i], functions[i]);
            __atomic_store_n(&hooks[i], hook, __ATOMIC_RELEASE);
        }
        if (hooks[i] < 0) {
            int error = hooks[i];
            vco_touch_kernel_stop();
            return error;
        }
    }
    SceTouchPanelInfo info;
    for (unsigned port = 0; port < 2; ++port) {
        if (ksceTouchGetPanelInfo(port, &info) >= 0 && info.maxAaX > info.minAaX &&
            info.maxAaY > info.minAaY) {
            available |= 1u << port;
            if (!port)
                front_panel = info;
        }
    }
    __atomic_store_n(&enabled, 1, __ATOMIC_RELEASE);
    return 0;
}
void vco_touch_kernel_sample(VcoTouchSample *out)
{
    *out = (VcoTouchSample){0};
    out->abi = VCO_TOUCH_ABI;
    out->tick = ksceKernelGetSystemTimeLow();
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return;
    out->available = available;
    for (unsigned port = 0; port < 2; ++port) {
        if (!(available & (1u << port)))
            continue;
        SceTouchData data = {0};
        /* Continue our own chain directly: no global bypass flag and no race
         * allowing another process's touch call to evade interception. */
        int result = TAI_CONTINUE(int, refs[0], port, &data, 1);
        if (result != 1 || data.reportNum > SCE_TOUCH_MAX_REPORT)
            continue;
        if (vco_kernel_capturing()) {
            __atomic_store_n(&captured_stamp[port], (uint32_t)data.timeStamp, __ATOMIC_RELEASE);
            __atomic_store_n(&cutoff_deadline[port], out->tick + 2000000u, __ATOMIC_RELEASE);
        }
        out->valid |= 1u << port;
        if (port) {
            out->back_count = data.reportNum;
            continue;
        }
        out->front_count = data.reportNum;
        if (data.reportNum == 1) {
            out->id = data.report[0].id;
            if (!vco_touch_map(data.report[0].x, front_panel.minDispX, front_panel.maxDispX,
                               VCO_SCREEN_WIDTH, &out->x) ||
                !vco_touch_map(data.report[0].y, front_panel.minDispY, front_panel.maxDispY,
                               VCO_SCREEN_HEIGHT, &out->y))
                out->x = VCO_SCREEN_WIDTH, out->y = VCO_SCREEN_HEIGHT;
        }
    }
}
