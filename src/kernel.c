#include "version.h"
#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>
#include <stddef.h>
#include "bridge.h"
#include "capture_policy.h"
#include "platform/game_probe.h"
#include "platform/touch_kernel.h"
#include "platform/vita/service.h"
#include "agent/native_loader.h"
#include "execution/mailbox.h"
void vco_probe_init(void);
void vco_probe_sample(int32_t pid, VcoGameProbe *out);
#include "diagnostics.h"

#define HOOKS 12
static tai_hook_ref_t refs[HOOKS];
static SceUID hooks[HOOKS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
enum { INPUT_NEW, INPUT_STARTING, INPUT_READY, INPUT_DEGRADED, INPUT_FAILED };
static uint32_t input_state;
static uint32_t capture, pulse, capture_epoch;

int vco_kernel_input_ready(void)
{
    return __atomic_load_n(&input_state, __ATOMIC_ACQUIRE) == INPUT_READY;
}
int vco_kernel_capturing(void)
{
    if (!vco_kernel_input_ready())
        return 0;
    uint32_t mode = __atomic_load_n(&capture, __ATOMIC_ACQUIRE);
    if (!mode)
        return 0;
    uint32_t elapsed = ksceKernelGetSystemTimeLow() - __atomic_load_n(&pulse, __ATOMIC_ACQUIRE);
    if (vco_capture_expired(mode, elapsed)) {
        __atomic_store_n(&capture, 0, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

uint32_t vco_kernel_capture_epoch(void)
{
    return __atomic_load_n(&capture_epoch, __ATOMIC_ACQUIRE);
}
/* Service worker calls this only after closing admission and draining all
 * complete input/RPC scopes during a Shell ownership transition. */
void vco_kernel_capture_reset(void)
{
    __atomic_store_n(&capture, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pulse, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&capture_epoch, 1, __ATOMIC_RELEASE);
}
int vcoPollTouch(VcoTouchSample *out)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int result = -1;
    int admission = out && vco_kernel_input_ready()
                        ? vco_service_call_begin(ksceKernelGetProcessId())
                        : VCO_MAILBOX_CLOSED;
    if (!admission) {
        VcoTouchSample sample;
        vco_touch_kernel_sample(&sample);
        result = ksceKernelCopyToUser(out, &sample, sizeof(sample));
        vco_service_call_end();
    } else if (admission == VCO_MAILBOX_BUSY)
        result = VCO_INPUT_BUSY;
    EXIT_SYSCALL(syscall_state);
    return result;
}

int vcoPoll(VcoInput *out)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    SceUID pid = ksceKernelGetProcessId();
    int ret = -1;
    int admission =
        out && vco_kernel_input_ready() ? vco_service_call_begin(pid) : VCO_MAILBOX_CLOSED;
    if (!admission) {
        SceCtrlData pad;
        unsigned char *pad_bytes = (unsigned char *)&pad;
        for (unsigned i = 0; i < sizeof(pad); ++i)
            pad_bytes[i] = 0;
        ret = ksceCtrlPeekBufferPositive(0, &pad, 1);
        static unsigned sample_logs;
        static int last_result;
        if (sample_logs == 0 || (ret != last_result && sample_logs < 12)) {
            vco_log("raw_ctrl_result", (unsigned)ret, 0);
            last_result = ret;
            ++sample_logs;
        }
        if (ret > 0) {
            VcoInput result;
            unsigned char *result_bytes = (unsigned char *)&result;
            for (unsigned i = 0; i < sizeof(result); ++i)
                result_bytes[i] = 0;
            result.abi = VCO_ABI;
            result.buttons = pad.buttons;
            result.tick = ksceKernelGetSystemTimeLow();
            result.captured = vco_kernel_capturing();
            result.lx = pad.lx;
            result.ly = pad.ly;
            result.rx = pad.rx;
            result.ry = pad.ry;
            ret = ksceKernelCopyToUser(out, &result, sizeof(result));
            /* An expired lease is never implicitly re-enabled by a heartbeat. */
            if (ret == 0 && result.captured &&
                vco_capture_renewable(__atomic_load_n(&capture, __ATOMIC_ACQUIRE)))
                __atomic_store_n(&pulse, result.tick, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&capture, 0, __ATOMIC_RELEASE);
            ret = -1;
        }
        vco_service_call_end();
    } else if (admission == VCO_MAILBOX_BUSY)
        ret = VCO_INPUT_BUSY;
    EXIT_SYSCALL(syscall_state);
    return ret;
}
int vcoCapture(int enable)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = -1;
    int admission =
        vco_kernel_input_ready() && enable >= VCO_CAPTURE_OFF && enable <= VCO_CAPTURE_DRAIN
            ? vco_service_call_begin(ksceKernelGetProcessId())
            : VCO_MAILBOX_ARGUMENT;
    if (!admission) {
        __atomic_add_fetch(&capture_epoch, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&pulse, ksceKernelGetSystemTimeLow(), __ATOMIC_RELEASE);
        __atomic_store_n(&capture, (uint32_t)enable, __ATOMIC_RELEASE);
        ret = 0;
        vco_service_call_end();
    } else if (admission == VCO_MAILBOX_BUSY)
        ret = VCO_INPUT_BUSY;
    EXIT_SYSCALL(syscall_state);
    return ret;
}

int vcoProbeGame(int32_t pid, VcoGameProbe *out)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int result = -1;
    int admission = vco_kernel_input_ready() && out && pid > 0 && pid != ksceKernelGetProcessId()
                        ? vco_service_call_begin(ksceKernelGetProcessId())
                        : VCO_MAILBOX_ARGUMENT;
    if (!admission) {
        VcoGameProbe probe;
        vco_probe_sample(pid, &probe);
        result = ksceKernelCopyToUser(out, &probe, sizeof(probe));
        vco_service_call_end();
    } else if (admission == VCO_MAILBOX_BUSY)
        result = VCO_INPUT_BUSY;
    EXIT_SYSCALL(syscall_state);
    return result;
}

static int filter_pad(SceCtrlData *data, int ret, int requested, int negative)
{
    if (ret <= 0 || !vco_kernel_capturing())
        return ret;
    if (ret > requested || ret > 64)
        return 0;
    for (int i = 0; i < ret; ++i) {
        SceCtrlData sample;
        if (ksceKernelCopyFromUser(&sample, &data[i], sizeof(sample)) < 0)
            return 0;
        uint32_t physical = negative ? ~sample.buttons : sample.buttons;
        /* Preserve the complete system screenshot chord; ordinary START
         * remains captured so it cannot affect the game under the overlay. */
        physical &= (physical & SCE_CTRL_PSBUTTON) ? (SCE_CTRL_PSBUTTON | SCE_CTRL_START)
                                                   : SCE_CTRL_PSBUTTON;
        sample.buttons = negative ? ~physical : physical;
        sample.lx = sample.ly = sample.rx = sample.ry = 128;
        sample.up = sample.right = sample.down = sample.left = 0;
        sample.lt = sample.rt = sample.l1 = sample.r1 = 0;
        sample.triangle = sample.circle = sample.cross = sample.square = 0;
        if (ksceKernelCopyToUser(&data[i], &sample, sizeof(sample)) < 0)
            return 0;
    }
    return ret;
}
#define PAD_HOOK(n, neg)                                                                           \
    static int pad_##n(int port, SceCtrlData *data, int count)                                     \
    {                                                                                              \
        int ret = TAI_CONTINUE(int, refs[n], port, data, count);                                   \
        return filter_pad(data, ret, count, neg);                                                  \
    }
// clang-format off
PAD_HOOK(0, 1)
PAD_HOOK(1, 1)
PAD_HOOK(2, 0)
PAD_HOOK(3, 0)
PAD_HOOK(4, 1)
PAD_HOOK(5, 1)
PAD_HOOK(6, 0)
PAD_HOOK(7, 0)
PAD_HOOK(8, 0)
PAD_HOOK(9, 0)
PAD_HOOK(10, 0)
PAD_HOOK(11, 0)
// clang-format on

static int startup_failed(int error, bool installed_any)
{
    __atomic_store_n(&input_state, installed_any ? INPUT_DEGRADED : INPUT_FAILED, __ATOMIC_RELEASE);
    vco_kernel_capture_reset();
    vco_touch_kernel_stop();
    vco_log("input_start_error", (unsigned)error, 0);
    vco_log("input_degraded_resident", installed_any, 0);
    /* No rollback release: TAI_CONTINUE may still reference a hook on another
     * CPU, and a negative release cannot prove whether its identity is reusable.
     * Keep every installed hook/ref and this code resident until reboot.
     * Readiness gates every helper/backend entry; surviving hooks only forward. */
    return installed_any ? SCE_KERNEL_START_SUCCESS : SCE_KERNEL_START_FAILED;
}
int module_start(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    uint32_t fresh = INPUT_NEW;
    if (!__atomic_compare_exchange_n(&input_state, &fresh, INPUT_STARTING, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return fresh == INPUT_FAILED ? SCE_KERNEL_START_FAILED : SCE_KERNEL_START_SUCCESS;
    vco_log("kernel_start_" VCO_VERSION, 0, 1);
    vco_probe_init();
    const uint32_t nids[HOOKS] = {0x104ED1A7, 0x81A89660, 0xA9C3CED6, 0x15F81E8C,
                                  0x15F96FB0, 0x27A0C5FB, 0x67E7AB83, 0xC4226A3E,
                                  0xA59454D3, 0x860BF292, 0xE2D99296, 0xA7178860};
    const void *funcs[HOOKS] = {pad_0, pad_1, pad_2, pad_3, pad_4,  pad_5,
                                pad_6, pad_7, pad_8, pad_9, pad_10, pad_11};
    bool installed_any = false;
    for (int i = 0; i < HOOKS; ++i) {
        SceUID hook = taiHookFunctionExportForKernel(KERNEL_PID, &refs[i], "SceCtrl", 0xD197E3C7,
                                                     nids[i], funcs[i]);
        __atomic_store_n(&hooks[i], hook, __ATOMIC_RELEASE);
        vco_log("hook_nid", nids[i], 0);
        vco_log("hook_result", (unsigned)hooks[i], 0);
        if (hooks[i] < 0) {
            /* Extended exports are absent on some firmware revisions.
             * The eight base read/peek APIs remain mandatory. */
            if (i >= 8)
                continue;
            return startup_failed(hooks[i], installed_any);
        }
        installed_any = true;
    }
    int touch_result = vco_touch_kernel_start();
    vco_log("touch_hooks_result", (unsigned)touch_result, 0);
    if (touch_result < 0)
        return startup_failed(touch_result, installed_any);
    __atomic_store_n(&input_state, INPUT_READY, __ATOMIC_RELEASE);
    int loader_result = vco_agent_loader_start();
    vco_log("game_loader_result", (unsigned)loader_result, 0);
    /* Input and fatal-status reporting stay available if game loading cannot
     * start. Any installed hooks/event handlers keep this module resident. */
    vco_log("kernel_ready", 0, 0);
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    /* User import references remain live; update/remove through reboot. */
    return SCE_KERNEL_STOP_CANCEL;
}
