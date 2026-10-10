#include "native_access.h"
#include "native_service.h"
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/ctrl.h>
#include <taihen.h>
#include <string.h>
int module_get_export_func(SceUID, const char *, uint32_t, uint32_t, uintptr_t *);
extern int vco_kernel_capturing(void);
extern uint32_t vco_kernel_capture_epoch(void);
static int (*module_list)(SceUID, int, int, SceUID *, SceSize *);
static int (*module_info)(SceUID, SceUID, SceKernelModuleInfo *);
static uint32_t phase;
static int failure;
static uintptr_t resolve(const uint32_t nids[2])
{
    const uint32_t libraries[2] = {0xC445FA63, 0x92C9FFC2};
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t address = 0;
        if (module_get_export_func(KERNEL_PID, "SceKernelModulemgr", libraries[i], nids[i],
                                   &address) >= 0 &&
            address)
            return address;
    }
    return 0;
}
int vco_agent_access_start(void)
{
    uint32_t empty = 0;
    if (!__atomic_compare_exchange_n(&phase, &empty, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return empty == 2 ? 0 : empty == 3 ? failure : -1;
    const uint32_t lists[2] = {0x97CF7B4E, 0xB72C75A4};
    const uint32_t infos[2] = {0xD269F915, 0xDAA90093};
    module_list = (int (*)(SceUID, int, int, SceUID *, SceSize *))resolve(lists);
    module_info = (int (*)(SceUID, SceUID, SceKernelModuleInfo *))resolve(infos);
    failure = module_list && module_info ? 0 : -1;
    __atomic_store_n(&phase, failure ? 3 : 2, __ATOMIC_RELEASE);
    return failure;
}
static bool range(const void *pointer, uint32_t size)
{
    uintptr_t address = (uintptr_t)pointer;
    return size && address >= 0x40000000u && address <= UINT32_MAX &&
           (uint64_t)address + size <= UINT64_C(0x100000000);
}
static bool scalar(uint32_t address, uint32_t width)
{
    return (width == 1 || width == 2 || width == 4) && range((void *)(uintptr_t)address, width);
}
static VcoAgentStatus output(void *out, const void *value, uint32_t length, uint32_t expected)
{
    if (length != expected || !range(out, length))
        return VCO_AGENT_ARGUMENT;
    return ksceKernelCopyToUser(out, value, length) < 0 ? VCO_AGENT_COPY : VCO_AGENT_OK;
}
static VcoAgentStatus request_copy(void *out, const void *input, uint32_t length, uint32_t expected)
{
    if (length != expected || !range(input, length))
        return VCO_AGENT_ARGUMENT;
    return ksceKernelCopyFromUser(out, input, length) < 0 ? VCO_AGENT_COPY : VCO_AGENT_OK;
}
static VcoAgentStatus memory_request(VcoAgentMemoryRequest *out, const VcoAgentMemoryRequest *input,
                                     uint32_t length)
{
    VcoAgentStatus error = request_copy(out, input, length, sizeof(*out));
    if (!error && (out->abi != VCO_AGENT_ABI || out->reserved || out->buffer > UINT32_MAX))
        error = VCO_AGENT_ARGUMENT;
    return error;
}
int vcoAgentRead(const VcoAgentMemoryRequest *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentMemoryRequest request;
    VcoAgentStatus error = memory_request(&request, input, length);
    if (error) {
        EXIT_SYSCALL(state);
        return error;
    }
    uint64_t session = request.session;
    uint32_t address = request.address, width = request.width;
    void *out = (void *)(uintptr_t)request.buffer;
    error = vco_agent_game_call_begin(session, true);
    if (!error) {
        uint8_t bytes[4];
        if (!scalar(address, width) || !range(out, width))
            error = VCO_AGENT_ARGUMENT;
        else if (ksceKernelCopyFromUser(bytes, (const void *)(uintptr_t)address, width) < 0)
            error = VCO_AGENT_IO;
        if (!error)
            error = vco_agent_game_call_check(session, true);
        if (!error)
            error = output(out, bytes, width, width);
        vco_agent_game_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoAgentWrite(const VcoAgentMemoryRequest *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentMemoryRequest request;
    VcoAgentStatus error = memory_request(&request, input, length);
    if (error) {
        EXIT_SYSCALL(state);
        return error;
    }
    uint64_t session = request.session;
    uint32_t address = request.address, width = request.width;
    const void *buffer = (const void *)(uintptr_t)request.buffer;
    error = vco_agent_game_call_begin(session, true);
    if (!error) {
        uint8_t bytes[4];
        if (!scalar(address, width) || !range(buffer, width))
            error = VCO_AGENT_ARGUMENT;
        else if (ksceKernelCopyFromUser(bytes, buffer, width) < 0)
            error = VCO_AGENT_COPY;
        if (!error)
            error = vco_agent_game_call_check(session, true);
        if (!error && ksceKernelCopyToUser((void *)(uintptr_t)address, bytes, width) < 0)
            error = VCO_AGENT_IO;
        if (!error)
            error = vco_agent_game_call_check(session, true);
        vco_agent_game_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoAgentModules(uint64_t session, VcoGameModuleList *out, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentStatus error = vco_agent_game_call_begin(session, false);
    if (!error) {
        VcoGameModuleList list = {0};
        SceSize count = VCO_GAME_MODULE_LIMIT + 1u;
        if (length != sizeof(list) || !range(out, length))
            error = VCO_AGENT_ARGUMENT;
        else if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != 2 ||
                 module_list(ksceKernelGetProcessId(), 0xff, 1, list.ids, &count) < 0)
            error = VCO_AGENT_IO;
        else if (count > VCO_GAME_MODULE_LIMIT)
            error = VCO_AGENT_LIMIT;
        if (!error) {
            list.count = count;
            for (unsigned i = 0; i < count && !error; ++i) {
                if (list.ids[i] <= 0)
                    error = VCO_AGENT_IO;
                for (unsigned j = 0; j < i && !error; ++j)
                    if (list.ids[i] == list.ids[j])
                        error = VCO_AGENT_IO;
            }
        }
        if (!error)
            error = vco_agent_game_call_check(session, false);
        if (!error)
            error = output(out, &list, length, sizeof(list));
        vco_agent_game_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoAgentModuleInfo(const VcoAgentModuleRequest *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentModuleRequest request;
    VcoAgentStatus error = request_copy(&request, input, length, sizeof(request));
    if (!error && (request.abi != VCO_AGENT_ABI || request.reserved || request.output > UINT32_MAX))
        error = VCO_AGENT_ARGUMENT;
    if (error) {
        EXIT_SYSCALL(state);
        return error;
    }
    uint64_t session = request.session;
    int32_t uid = request.uid;
    VcoGameModuleInfo *out = (VcoGameModuleInfo *)(uintptr_t)request.output;
    length = request.length;
    error = vco_agent_game_call_begin(session, false);
    if (!error) {
        SceKernelModuleInfo native = {0};
        VcoGameModuleInfo value = {.uid = uid};
        SceUID membership[VCO_GAME_MODULE_LIMIT + 1u];
        SceSize count = VCO_GAME_MODULE_LIMIT + 1u;
        native.size = sizeof(native);
        if (uid <= 0 || length != sizeof(value) || !range(out, length))
            error = VCO_AGENT_ARGUMENT;
        else if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != 2 ||
                 module_list(ksceKernelGetProcessId(), 0xff, 1, membership, &count) < 0)
            error = VCO_AGENT_IO;
        else if (count > VCO_GAME_MODULE_LIMIT)
            error = VCO_AGENT_LIMIT;
        if (!error) {
            bool found = false;
            for (unsigned i = 0; i < count; ++i)
                found |= membership[i] == uid;
            if (!found)
                error = VCO_AGENT_ARGUMENT;
        }
        if (!error && module_info(ksceKernelGetProcessId(), uid, &native) < 0)
            error = VCO_AGENT_IO;
        if (!error && (native.modid <= 0 || !native.module_name[0] ||
                       !memchr(native.module_name, 0, sizeof(native.module_name))))
            error = VCO_AGENT_IO;
        if (!error) {
            value.process_uid = native.modid;
            value.attributes = native.modattr;
            value.version = (uint32_t)native.modver[0] | (uint32_t)native.modver[1] << 8;
            value.state = native.state;
            /* Canonical trailing string bytes prevent false snapshot changes. */
            memcpy(value.name, native.module_name, strlen(native.module_name));
            for (unsigned i = 0; i < 4; ++i) {
                uintptr_t base = (uintptr_t)native.segments[i].vaddr;
                uint32_t size = native.segments[i].memsz;
                if (base > UINT32_MAX || (size && !range((void *)base, size))) {
                    error = VCO_AGENT_IO;
                    break;
                }
                value.segments[i] =
                    (VcoGameModuleSegment){(uint32_t)base, size, native.segments[i].perms};
            }
        }
        if (!error)
            error = vco_agent_game_call_check(session, false);
        if (!error)
            error = output(out, &value, length, sizeof(value));
        vco_agent_game_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
int vcoAgentPad(uint64_t session, VcoGamePadSample *out, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    VcoAgentStatus error = vco_agent_game_call_begin(session, true);
    if (!error) {
        VcoGamePadSample value = {0};
        SceCtrlData pad = {0};
        SceCtrlPortInfo first = {0}, second = {0};
        uint32_t epoch = vco_kernel_capture_epoch();
        int before = vco_kernel_capturing();
        if (length != sizeof(value) || !range(out, length))
            error = VCO_AGENT_ARGUMENT;
        else if (ksceCtrlGetControllerPortInfo(&first) ||
                 ksceCtrlPeekBufferPositive(0, &pad, 1) != 1 ||
                 ksceCtrlGetControllerPortInfo(&second) ||
                 memcmp(first.port, second.port, sizeof(first.port)))
            error = VCO_AGENT_IO;
        for (unsigned i = 0; i < sizeof(first.port) && !error; ++i)
            if (first.port[i] != SCE_CTRL_TYPE_UNPAIRED && first.port[i] != SCE_CTRL_TYPE_PHY &&
                first.port[i] != SCE_CTRL_TYPE_VIRT && first.port[i] != SCE_CTRL_TYPE_DS3 &&
                first.port[i] != SCE_CTRL_TYPE_DS4)
                error = VCO_AGENT_IO;
        int after = vco_kernel_capturing();
        value.capture_epoch = vco_kernel_capture_epoch();
        value.capturing = before || after || epoch != value.capture_epoch;
        value.stamp = pad.timeStamp;
        value.buttons = pad.buttons;
        value.device = first.port[0];
        if (!error)
            error = vco_agent_game_call_check(session, true);
        if (!error)
            error = output(out, &value, length, sizeof(value));
        vco_agent_game_call_end();
    }
    EXIT_SYSCALL(state);
    return error;
}
