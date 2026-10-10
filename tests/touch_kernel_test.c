#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <taihen.h>
#include "platform/touch_kernel.h"
static const void *installed[4];
static const uint32_t nids[] = {0xBAD1960B, 0x70C8AACE, 0x9B3F7207, 0x9A91F624};
static uint32_t now = 100000, epoch, stamp = 1234;
static bool captured, change_epoch, fail_panel;
static bool stop_in_continue;
static int fail_hook = -1, override_result, released, last_region;
static unsigned installs, continues;
uint32_t ksceKernelGetSystemTimeLow(void)
{
    return now;
}
int vco_kernel_capturing(void)
{
    return captured;
}
uint32_t vco_kernel_capture_epoch(void)
{
    return epoch;
}
int ksceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo *out)
{
    (void)port;
    if (fail_panel)
        return -1;
    *out = (SceTouchPanelInfo){0};
    out->maxAaX = out->maxDispX = 1919;
    out->maxAaY = out->maxDispY = 1087;
    return 0;
}
int taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module,
                                   uint32_t library, uint32_t nid, const void *function)
{
    assert(pid == KERNEL_PID && library == 0xA4A7DAFC);
    assert(!strcmp(module, "SceTouch") || !strcmp(module, "SceTouchDummy"));
    unsigned index = 0;
    while (index < 4 && nids[index] != nid)
        ++index;
    assert(index < 4 && !installed[index]);
    ++installs;
    if ((int)index == fail_hook)
        return -1;
    installed[index] = function;
    *ref = index;
    return (int)index + 100;
}
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref)
{
    assert(ref < 4 && uid == (int)ref + 100 && installed[ref]);
    ++released;
    return -77; /* Neither success nor ambiguous failure may be reached by hot cleanup. */
}
int fake_touch_continue(tai_hook_ref_t ref, unsigned port, SceTouchData *data, unsigned count, ...)
{
    assert(ref < 4 && installed[ref]);
    ++continues;
    if (stop_in_continue)
        vco_touch_kernel_stop();
    assert(installed[ref] && !released);
    if (ref >= 2) {
        va_list args;
        va_start(args, count);
        last_region = va_arg(args, int);
        va_end(args);
    }
    if (change_epoch)
        ++epoch;
    if (override_result)
        return override_result;
    for (unsigned i = 0; i < count; ++i) {
        data[i] = (SceTouchData){0};
        data[i].timeStamp = stamp;
        data[i].reportNum = port ? 2 : 1;
        memset(data[i].report, 0x5a, sizeof(data[i].report));
        data[i].report[0].x = 1919;
        data[i].report[0].y = 1087;
        data[i].report[0].id = 7;
    }
    return (int)count;
}
static int call(unsigned hook, unsigned port, SceTouchData *data, unsigned count)
{
    assert(hook < 4 && installed[hook]);
    if (hook < 2)
        return ((int (*)(unsigned, SceTouchData *, unsigned))installed[hook])(port, data, count);
    return ((int (*)(unsigned, SceTouchData *, unsigned, int))installed[hook])(port, data, count,
                                                                               42);
}
int main(int argc, char **argv)
{
    assert(sizeof(SceTouchData) == 0x90 && sizeof(SceTouchPanelInfo) == 0x30);
    if (argc == 2 && !strncmp(argv[1], "fail", 4)) {
        int failed = argv[1][4] - '0';
        assert(failed >= 0 && failed < 4 && !argv[1][5]);
        fail_hook = failed;
        assert(vco_touch_kernel_start() < 0);
        assert(installs == (failed ? (unsigned)failed + 1 : 2));
        for (unsigned i = 0; i < 4; ++i)
            assert((installed[i] != NULL) == ((int)i < failed));
        VcoTouchSample raw;
        captured = true;
        vco_touch_kernel_sample(&raw);
        assert(!raw.available && !raw.valid && !continues);
        for (int i = 0; i < failed; ++i) {
            SceTouchData data;
            for (unsigned port = 0; port < 2; ++port) {
                change_epoch = true;
                assert(call((unsigned)i, port, &data, 1) == 1);
                assert(data.reportNum == (port ? 2 : 1) && data.report[0].id == 7 &&
                       data.timeStamp == stamp);
            }
            override_result = 65;
            assert(call((unsigned)i, 0, &data, 1) == 65);
            override_result = -7;
            assert(call((unsigned)i, 0, &data, 1) == -7);
            override_result = 0;
        }
        vco_touch_kernel_stop();
        vco_touch_kernel_stop();
        unsigned attempts = installs;
        if (failed)
            fail_hook = -1; /* A partial resident instance cannot retry even after recovery. */
        assert(vco_touch_kernel_start() < 0 && !released);
        assert(installs == attempts + (failed ? 0u : 2u));
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "no_panel")) {
        fail_panel = true;
        assert(!vco_touch_kernel_start());
        VcoTouchSample raw;
        vco_touch_kernel_sample(&raw);
        assert(!raw.available && !raw.valid && vco_touch_neutral(&raw) && !continues);
        vco_touch_kernel_stop();
        assert(!released && vco_touch_kernel_start() < 0);
        return 0;
    }
    assert(argc == 1);
    fail_hook = -1;
    assert(!vco_touch_kernel_start());
    assert(installs == 4 && !vco_touch_kernel_start() && installs == 4);
    SceTouchData data[2];
    for (unsigned hook = 0; hook < 4; ++hook) {
        for (unsigned port = 0; port < 2; ++port) {
            captured = false;
            assert(call(hook, port, data, 2) == 2 && data[0].reportNum == (port ? 2 : 1));
            captured = true;
            assert(call(hook, port, data, 2) == 2);
            assert(!data[0].reportNum && !data[1].reportNum && data[0].timeStamp == stamp);
            for (unsigned i = 0; i < sizeof(data[0].report); ++i)
                assert(!((uint8_t *)data[0].report)[i]);
        }
    }
    assert(last_region == 42);
    VcoTouchSample raw;
    vco_touch_kernel_sample(&raw);
    assert(raw.available == 3 && raw.valid == 3 && raw.front_count == 1 && raw.back_count == 2);
    assert(raw.x == 959 && raw.y == 543 &&
           raw.id == 7); /* direct continuation bypasses filtering */
    captured = false;
    assert(call(0, 0, data, 1) == 1 && !data[0].reportNum); /* retained capture history */
    ++stamp;
    assert(call(0, 0, data, 1) == 1 && data[0].reportNum == 1);
    change_epoch = true;
    assert(call(0, 0, data, 1) == 1 && !data[0].reportNum);
    change_epoch = false;
    captured = true;
    override_result = 65;
    assert(call(0, 0, data, 2) == 0);
    override_result = -7;
    assert(call(0, 0, data, 2) == -7);
    vco_touch_kernel_sample(&raw);
    assert(raw.valid == 0);
    override_result = 0;
    captured = false;
    now += 3000000;
    assert(call(0, 1, data, 2) == 2 && data[0].reportNum == 2);
    captured = true;
    stop_in_continue = true;
    assert(call(0, 0, data, 1) == 1 && data[0].reportNum == 1);
    stop_in_continue = false;
    unsigned samples = continues;
    vco_touch_kernel_sample(&raw);
    assert(raw.available == 0 && vco_touch_neutral(&raw) && samples == continues);
    vco_touch_kernel_stop();
    assert(vco_touch_kernel_start() < 0 && !released);
    for (unsigned i = 0; i < 4; ++i) {
        assert(installed[i]);
        assert(call(i, 0, data, 1) == 1 && data[0].reportNum == 1);
        assert(call(i, 1, data, 1) == 1 && data[0].reportNum == 2);
    }
    assert(installs == 4 && !released);
    return 0;
}
