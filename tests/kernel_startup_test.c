#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/modulemgr.h>
#include <taihen.h>
#include "bridge.h"
#include "platform/touch_kernel.h"
#include "platform/game_probe.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int module_start(SceSize, void *);
int module_stop(SceSize, void *);
int vco_kernel_input_ready(void);
int vcoProbeGame(int32_t, VcoGameProbe *);
int vcoPollTouch(VcoTouchSample *);
static const uint32_t nids[] = {
    0x104ED1A7, 0x81A89660, 0xA9C3CED6, 0x15F81E8C, 0x15F96FB0, 0x27A0C5FB, 0x67E7AB83, 0xC4226A3E,
    0xA59454D3, 0x860BF292, 0xE2D99296, 0xA7178860, 0xBAD1960B, 0x70C8AACE, 0x9B3F7207, 0x9A91F624};
static const void *installed[16];
static int failure = -1;
static bool optional_missing, stop_in_continue;
static unsigned attempts, releases, admissions, scopes, copies, probes, driver_reads, syscall_depth;
static uint32_t now = 100000;
static uint32_t pad_buttons = SCE_CTRL_CIRCLE | SCE_CTRL_PSBUTTON;
static unsigned game_loaders;
int vco_agent_loader_start(void)
{
    assert(vco_kernel_input_ready());
    ++game_loaders;
    return 0;
}
uint32_t fake_enter_syscall(void)
{
    assert(!syscall_depth);
    syscall_depth = 1;
    return 42;
}
void fake_exit_syscall(uint32_t state)
{
    assert(state == 42 && syscall_depth == 1 && !scopes);
    syscall_depth = 0;
}
uint32_t ksceKernelGetSystemTimeLow(void)
{
    return now;
}
int32_t ksceKernelGetProcessId(void)
{
    return 77;
}
int vco_service_call_begin(int32_t caller)
{
    assert(caller == 77 && vco_kernel_input_ready() && !scopes);
    ++admissions;
    ++scopes;
    return 0;
}
void vco_service_call_end(void)
{
    assert(scopes == 1);
    --scopes;
}
int ksceKernelCopyToUser(void *out, const void *in, size_t length)
{
    ++copies;
    memcpy(out, in, length);
    return 0;
}
int ksceKernelCopyFromUser(void *out, const void *in, size_t length)
{
    ++copies;
    memcpy(out, in, length);
    return 0;
}
void vco_probe_init(void)
{
    ++probes;
}
void vco_probe_sample(int32_t pid, VcoGameProbe *out)
{
    assert(pid == 88 && scopes);
    *out = (VcoGameProbe){.abi = VCO_PROBE_ABI};
}
int vco_diagnostics_write(const char *path, const char *event, unsigned value, int reset)
{
    (void)value;
    (void)reset;
    assert(path && event);
    return 0;
}
static void pad_data(SceCtrlData *out, bool negative)
{
    *out = (SceCtrlData){.buttons = pad_buttons, .lx = 20, .ly = 30, .rx = 40, .ry = 50};
    if (negative)
        out->buttons = ~out->buttons;
}
int ksceCtrlPeekBufferPositive(int port, SceCtrlData *out, int count)
{
    assert(!port && count == 1);
    ++driver_reads;
    pad_data(out, false);
    return 1;
}
int ksceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo *out)
{
    assert(port < 2);
    *out = (SceTouchPanelInfo){.maxAaX = 1919, .maxAaY = 1087, .maxDispX = 1919, .maxDispY = 1087};
    return 0;
}
static bool negative(unsigned index)
{
    return index == 0 || index == 1 || index == 4 || index == 5;
}
int fake_kernel_continue(tai_hook_ref_t ref, unsigned port, void *data, unsigned count, ...)
{
    assert(ref < 16 && installed[ref] && port < 2);
    if (stop_in_continue)
        assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL);
    for (unsigned i = 0; i < count; ++i) {
        if (ref < 12)
            pad_data(&((SceCtrlData *)data)[i], negative((unsigned)ref));
        else {
            SceTouchData *touch = &((SceTouchData *)data)[i];
            *touch = (SceTouchData){.timeStamp = now, .reportNum = 1};
            touch->report[0].x = 100;
            touch->report[0].y = 200;
        }
    }
    return (int)count;
}
int taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref, const char *module,
                                   uint32_t library, uint32_t nid, const void *function)
{
    assert(pid == KERNEL_PID && !vco_kernel_input_ready());
    unsigned index = 0;
    while (index < 16 && nids[index] != nid)
        ++index;
    assert(index < 16 && !installed[index]);
    assert(library == (index < 12 ? 0xD197E3C7u : 0xA4A7DAFCu));
    assert(!strcmp(module, index < 12 ? "SceCtrl" : "SceTouch") ||
           (index == 12 && !strcmp(module, "SceTouchDummy")));
    ++attempts;
    if ((int)index == failure || (optional_missing && index >= 8 && index < 12))
        return -55;
    installed[index] = function;
    *ref = index;
    return (int)index + 100;
}
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref)
{
    assert(ref < 16 && uid == (int)ref + 100);
    ++releases;
    return -77; /* An ambiguous release must never be attempted on this lifecycle. */
}
static void helpers_rejected(void)
{
    VcoInput input, previous_input;
    VcoTouchSample touch, previous_touch;
    VcoGameProbe probe, previous_probe;
    memset(&input, 0x55, sizeof(input));
    previous_input = input;
    memset(&touch, 0x55, sizeof(touch));
    previous_touch = touch;
    memset(&probe, 0x55, sizeof(probe));
    previous_probe = probe;
    unsigned prior_copies = copies, prior_reads = driver_reads, prior_admissions = admissions;
    assert(vcoPoll(&input) < 0 && !memcmp(&input, &previous_input, sizeof(input)));
    assert(vcoPollTouch(&touch) < 0 && !memcmp(&touch, &previous_touch, sizeof(touch)));
    assert(vcoProbeGame(88, &probe) < 0 && !memcmp(&probe, &previous_probe, sizeof(probe)));
    assert(vcoCapture(VCO_CAPTURE_ACTIVE) < 0 && !vco_kernel_capturing());
    assert(copies == prior_copies && driver_reads == prior_reads && admissions == prior_admissions);
}
static void passthrough(void)
{
    for (unsigned i = 0; i < 16; ++i) {
        if (!installed[i])
            continue;
        if (i < 12) {
            SceCtrlData out, expected;
            pad_data(&expected, negative(i));
            assert(((int (*)(int, SceCtrlData *, int))installed[i])(0, &out, 1) == 1);
            assert(!memcmp(&out, &expected, sizeof(out)));
        } else {
            SceTouchData out;
            int result =
                i < 14 ? ((int (*)(unsigned, SceTouchData *, unsigned))installed[i])(0, &out, 1)
                       : ((int (*)(unsigned, SceTouchData *, unsigned, int))installed[i])(0, &out,
                                                                                          1, 42);
            assert(result == 1 && out.reportNum == 1);
        }
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strncmp(argv[1], "ctrl", 4)) {
        failure = argv[1][4] - '0';
        assert(failure >= 0 && failure < 8 && !argv[1][5]);
    } else if (!strncmp(argv[1], "touch", 5)) {
        failure = 12 + argv[1][5] - '0';
        assert(failure >= 12 && failure < 16 && !argv[1][6]);
    } else if (!strcmp(argv[1], "optional"))
        optional_missing = true;
    else if (!strcmp(argv[1], "inflight")) {
        failure = 3;
        stop_in_continue = true;
    } else
        assert(!strcmp(argv[1], "ready"));
    helpers_rejected();
    int expected = failure == 0 ? SCE_KERNEL_START_FAILED : SCE_KERNEL_START_SUCCESS;
    assert(module_start(0, NULL) == expected && probes == 1);
    unsigned installed_attempts = attempts;
    assert(module_start(0, NULL) == expected && attempts == installed_attempts && probes == 1);
    if (failure >= 0) {
        assert(!vco_kernel_input_ready());
        helpers_rejected();
        passthrough();
        assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL);
        helpers_rejected();
        passthrough();
    } else {
        assert(vco_kernel_input_ready());
        assert(!vcoCapture(VCO_CAPTURE_ACTIVE) && vco_kernel_capturing());
        VcoInput input;
        assert(!vcoPoll(&input) && input.abi == VCO_ABI && input.captured);
        SceCtrlData out;
        assert(((int (*)(int, SceCtrlData *, int))installed[2])(0, &out, 1) == 1);
        assert(out.buttons == SCE_CTRL_PSBUTTON && out.lx == 128 && out.ry == 128);
        pad_buttons = SCE_CTRL_PSBUTTON | SCE_CTRL_START | SCE_CTRL_CIRCLE;
        assert(((int (*)(int, SceCtrlData *, int))installed[2])(0, &out, 1) == 1);
        assert(out.buttons == (SCE_CTRL_PSBUTTON | SCE_CTRL_START));
        assert(((int (*)(int, SceCtrlData *, int))installed[0])(0, &out, 1) == 1);
        assert(~out.buttons == (SCE_CTRL_PSBUTTON | SCE_CTRL_START));
        pad_buttons = SCE_CTRL_START | SCE_CTRL_CIRCLE;
        assert(((int (*)(int, SceCtrlData *, int))installed[2])(0, &out, 1) == 1);
        assert(!out.buttons);

        VcoTouchSample touch;
        assert(!vcoPollTouch(&touch) && touch.available == 3 && touch.front_count == 1);
        SceTouchData filtered;
        assert(((int (*)(unsigned, SceTouchData *, unsigned))installed[12])(0, &filtered, 1) == 1);
        assert(!filtered.reportNum);
        assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL && vco_kernel_input_ready());
    }
    assert(!releases && !syscall_depth && !scopes);
    assert(game_loaders == (vco_kernel_input_ready() ? 1u : 0u));
    puts("kernel startup ownership passed");
    return 0;
}
