#define _POSIX_C_SOURCE 200809L
#include "agent/native_service.h"
#include "agent/native_loader.h"
#include "agent/executor.h"
#include "agent/native_access.h"
#include "agent/native_modules.h"
#include "platform/vita/service.h"
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/ctrl.h>
#include <taihen.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#define BASE UINT32_C(0x81000000)
static __thread int32_t caller = 99;
static int32_t shell_pid = 99;
static int64_t clock_us = 100000;
static __thread unsigned syscall_depth;
static unsigned copies_in, copies_out, registers, hook_calls, loads, continues;
static unsigned references, releases, capture_resets, module_starts;
static bool input_ready = true, title_missing, hook_first_fail, hook_both_fail, register_fail;
static bool copy_in_fail, copy_out_fail, load_fail, start_fail, title_exit, title_switch;
static bool copy_data_in_fail, mutate_request;
static int original_result = 42;
static unsigned native_module_count = 2, list_calls, info_calls, pad_calls;
static bool access_missing, access_first_missing, modules_fail, info_fail, copy_switch, info_exit;
static bool duplicate_module, invalid_module_name, empty_segment, bad_segment, topology_change;
static uint32_t capturing, capture_epoch, pad_buttons;
static SceClass process_class = {1};
static SceObjectBase shell_objects[2] = {{1}, {2}};
static unsigned shell_version;
static const SceProcEventHandler *events;
static int (*preloaded)(SceUID);
static int (*module_callbacks[8])(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static unsigned module_hook_calls, module_continues, module_delays;
static int module_fail_index = -1;
static bool module_drive, module_nested, module_negative, module_exit, module_real_concurrent;
static bool module_gate_fail;
static bool release_loader_gate;
static int (*file_callback)(SceUID, const char *, int, SceMode);
static unsigned file_hooks, file_original_calls, file_kernel_calls;
static bool file_hook_fail, file_drive, file_scope_checks, file_flag_checks, file_nested;
static bool require_loader_context;
static bool require_loader_process, permission_set_fail, pid_set_fail;
static __thread int loader_permission = 0x12;
static unsigned permission_sets, pid_sets;
static unsigned start_calls, cleanup_calls;
static bool start_sdk_fail, cleanup_fail;
static bool loader_exports_missing, loader_exports_first_missing;
static int file_original_result = -13, file_kernel_result = 64;
static bool module_quiescent_original, module_shutdown_on_delay;
static __thread bool module_original_active;
static uint32_t module_worker_stop;
static pthread_mutex_t module_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t module_condition = PTHREAD_COND_INITIALIZER;
static unsigned module_entered, module_release_mask;
static void execute_game(void);
static VcoAgentBinding binding;
static VcoGameExecutor executor;
static uint8_t game[8];
static bool initialized, fail_allocation;
typedef struct {
    uintptr_t address;
    void *data;
    size_t size;
} Mapping;
static Mapping maps[64];
static unsigned map_count;
static uintptr_t next_address = 0x50000000;
static VcoAgentMessage message;
static VcoAgentResult result;
static uint8_t payload[VCO_UPLOAD_MAX], packet[VCO_UPLOAD_MAX];
static size_t packet_length;
static void *user(void *data, size_t size)
{
    assert(map_count < 64 && size);
    uintptr_t address = next_address;
    next_address += ((size + 4095u) & ~(uintptr_t)4095u) + 4096;
    maps[map_count++] = (Mapping){address, data, size};
    return (void *)address;
}
static __thread const void *request_bytes;
static __thread size_t request_size;
static void *mapped(const void *pointer, size_t size)
{
    if ((uintptr_t)pointer == 0x70000000u && request_bytes && size <= request_size)
        return (void *)request_bytes;
    uintptr_t address = (uintptr_t)pointer;
    for (unsigned i = 0; i < map_count; ++i)
        if (address >= maps[i].address && address - maps[i].address <= maps[i].size &&
            size <= maps[i].size - (address - maps[i].address))
            return (uint8_t *)maps[i].data + (address - maps[i].address);
    return NULL;
}
/* Marshal exactly the raw requests. A thread-local synthetic user mapping
 * avoids allocating persistent mappings for transient syscall stack packets. */
static int game_poll(uint64_t session, VcoAgentMessage *out, uint32_t length, void *payload,
                     uint32_t capacity)
{
    VcoAgentPollRequest r = {.session = session,
                             .abi = VCO_AGENT_ABI,
                             .message = (uintptr_t)out,
                             .payload = (uintptr_t)payload,
                             .message_length = length,
                             .capacity = capacity};
    request_bytes = &r;
    request_size = sizeof(r);
    int error = vcoAgentPoll((const VcoAgentPollRequest *)0x70000000u, sizeof(r));
    request_bytes = NULL;
    request_size = 0;
    return error;
}
static int game_read(uint64_t session, uint32_t address, void *out, uint32_t width)
{
    VcoAgentMemoryRequest r = {.session = session,
                               .abi = VCO_AGENT_ABI,
                               .address = address,
                               .buffer = (uintptr_t)out,
                               .width = width};
    request_bytes = &r;
    request_size = sizeof(r);
    int error = vcoAgentRead((const VcoAgentMemoryRequest *)0x70000000u, sizeof(r));
    request_bytes = NULL;
    request_size = 0;
    return error;
}
static int game_write(uint64_t session, uint32_t address, const void *input, uint32_t width)
{
    VcoAgentMemoryRequest r = {.session = session,
                               .abi = VCO_AGENT_ABI,
                               .address = address,
                               .buffer = (uintptr_t)input,
                               .width = width};
    request_bytes = &r;
    request_size = sizeof(r);
    int error = vcoAgentWrite((const VcoAgentMemoryRequest *)0x70000000u, sizeof(r));
    request_bytes = NULL;
    request_size = 0;
    return error;
}
static int game_info(uint64_t session, int32_t uid, VcoGameModuleInfo *out, uint32_t length)
{
    VcoAgentModuleRequest r = {.session = session,
                               .abi = VCO_AGENT_ABI,
                               .uid = uid,
                               .output = (uintptr_t)out,
                               .length = length};
    request_bytes = &r;
    request_size = sizeof(r);
    int error = vcoAgentModuleInfo((const VcoAgentModuleRequest *)0x70000000u, sizeof(r));
    request_bytes = NULL;
    request_size = 0;
    return error;
}
uint32_t fake_enter_syscall(void)
{
    assert(!syscall_depth || (file_drive && syscall_depth == 1) ||
           (file_nested && syscall_depth == 2));
    uint32_t previous = syscall_depth;
    ++syscall_depth;
    return 0x55 + previous;
}
void fake_exit_syscall(uint32_t state)
{
    assert(syscall_depth && state == 0x55 + syscall_depth - 1);
    --syscall_depth;
}
int32_t ksceKernelGetProcessId(void)
{
    return caller;
}
int ksceKernelSetPermission(int value)
{
    assert(syscall_depth == 1 && value >= 0);
    ++permission_sets;
    if (permission_set_fail)
        return -71;
    int previous = loader_permission;
    loader_permission = value;
    return previous;
}
int32_t ksceKernelSetProcessIdToTLS(int32_t value)
{
    assert(syscall_depth == 1 && value > 0);
    ++pid_sets;
    if (pid_set_fail)
        return -72;
    int32_t previous = caller;
    caller = value;
    return previous;
}
int64_t ksceKernelGetSystemTimeWide(void)
{
    if (module_real_concurrent) {
        struct timespec stamp;
        assert(!clock_gettime(CLOCK_MONOTONIC, &stamp));
        return (int64_t)stamp.tv_sec * 1000000 + stamp.tv_nsec / 1000;
    }
    return clock_us;
}
SceUID ksceKernelSysrootGetShellPid(void)
{
    return shell_pid;
}
int vco_kernel_input_ready(void)
{
    return input_ready;
}
void vco_kernel_capture_reset(void)
{
    ++capture_resets;
}
int vco_kernel_capturing(void)
{
    return (int)capturing;
}
uint32_t vco_kernel_capture_epoch(void)
{
    return capture_epoch;
}
int ksceCtrlGetControllerPortInfo(SceCtrlPortInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->port[0] = SCE_CTRL_TYPE_PHY;
    if (topology_change && pad_calls)
        out->port[0] = SCE_CTRL_TYPE_DS4;
    return 0;
}
int ksceCtrlPeekBufferPositive(int port, SceCtrlData *out, int count)
{
    assert(port == 0 && count == 1);
    ++pad_calls;
    memset(out, 0, sizeof(*out));
    out->timeStamp = (uint64_t)clock_us;
    out->buttons = pad_buttons;
    return 1;
}
static int list_modules(SceUID pid, int flags, int kernel_uids, SceUID *ids, SceSize *count)
{
    assert(pid == 77 && flags == 0xff && kernel_uids == 1 && *count == 257);
    ++list_calls;
    if (modules_fail)
        return -1;
    *count = native_module_count;
    for (unsigned i = 0; i < native_module_count && i < 257; ++i)
        ids[i] = duplicate_module ? 11 : (int32_t)i + 11;
    return 0;
}
static int info_module(SceUID pid, SceUID uid, SceKernelModuleInfo *out)
{
    assert(pid == 77 && uid > 0 && out->size == sizeof(*out));
    ++info_calls;
    if (info_fail)
        return -1;
    out->modid = uid + 100; /* Kernel and process UID namespaces differ. */
    out->modattr = 3;
    out->modver[0] = 1;
    out->state = SCE_KERNEL_MODULE_STATE_STARTED;
    strcpy(out->module_name, "eboot");
    if (invalid_module_name)
        memset(out->module_name, 'x', sizeof(out->module_name));
    out->segments[0].vaddr = (void *)(uintptr_t)(bad_segment ? 0x10000000u : BASE);
    out->segments[0].memsz = empty_segment ? 0 : 8;
    out->segments[0].perms = 5;
    if (info_exit) {
        info_exit = false;
        events->exit(pid, NULL, 0);
    }
    return 0;
}
static SceClass *get_class(void)
{
    return &process_class;
}
static int get_title(int32_t pid, char *out, uint32_t size)
{
    assert(size == 32);
    const char *title = pid == 77 || pid == 78 ? "PCSB00031" : "NPXS10015";
    memset(out, 0, size);
    memcpy(out, title, 10);
    if (title_exit) {
        title_exit = false;
        events->exit(pid, NULL, 0);
    }
    if (title_switch) {
        title_switch = false;
        events->switch_process(0, 0, NULL, 0);
    }
    return 0;
}
int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *out)
{
    assert(pid == KERNEL_PID);
    if (!strcmp(module, "SceSysmem")) {
        assert(library == 0x3691DA45 && function == 0xEC3124A3);
        if (title_missing)
            return -1;
        *out = (uintptr_t)get_title;
        return 0;
    }
    if (!strcmp(module, "SceKernelModulemgr")) {
        assert(library == 0xC445FA63 || library == 0x92C9FFC2);
        if (function == 0xFA21D8CB || function == 0x4E85022D || function == 0x6DF745D5 ||
            function == 0x3FE47DDF) {
            bool first = library == 0xC445FA63;
            bool load = function == 0xFA21D8CB || function == 0x4E85022D;
            assert(function == (load ? (first ? 0xFA21D8CBu : 0x4E85022Du)
                                     : (first ? 0x6DF745D5u : 0x3FE47DDFu)));
            if (loader_exports_missing || (loader_exports_first_missing && first))
                return -1;
            *out = load ? (uintptr_t)ksceKernelLoadModuleForPid
                        : (uintptr_t)ksceKernelStartModuleForPid;
            return 0;
        }
        if (access_missing || (access_first_missing && library == 0xC445FA63))
            return -1;
        if (function == 0x97CF7B4E || function == 0xB72C75A4)
            *out = (uintptr_t)list_modules;
        else {
            assert(function == 0xD269F915 || function == 0xDAA90093);
            *out = (uintptr_t)info_module;
        }
        return 0;
    }
    assert(!strcmp(module, "SceProcessmgr"));
    assert(library == 0x7A69DE86 && function == 0xC6820972);
    *out = (uintptr_t)get_class;
    return 0;
}
int ksceGUIDReferObjectWithClass(SceUID pid, SceClass *class_pointer, SceObjectBase **out)
{
    /* No process reference for the game is acquired by the new backend. */
    assert((pid == 99 || pid == 100) && class_pointer == &process_class);
    ++references;
    *out = &shell_objects[shell_version];
    return 0;
}
int ksceGUIDReleaseObject(SceUID pid)
{
    assert(pid == 99 || pid == 100);
    ++releases;
    assert(releases <= references);
    return 0;
}
int ksceKernelCopyFromUser(void *out, const void *input, size_t size)
{
    assert(syscall_depth == 1);
    __atomic_add_fetch(&copies_in, 1u, __ATOMIC_RELAXED);
    if (copy_data_in_fail && (uintptr_t)input >= BASE && (uintptr_t)input < BASE + sizeof(game)) {
        copy_data_in_fail = false;
        return -8;
    }
    if (copy_in_fail) {
        copy_in_fail = false;
        return -8;
    }
    void *source = mapped(input, size);
    if (!source)
        return -9;
    memcpy(out, source, size);
    if (mutate_request && size == sizeof(VcoAgentMemoryRequest)) {
        mutate_request = false;
        memset(source, 0xcc, size);
    }
    if (copy_switch) {
        copy_switch = false;
        events->stop(caller, 0, NULL, 0);
    }
    return 0;
}
int ksceKernelCopyToUser(void *out, const void *input, size_t size)
{
    assert(syscall_depth == 1);
    ++copies_out;
    if (copy_out_fail) {
        copy_out_fail = false;
        return -8;
    }
    void *target = mapped(out, size);
    if (!target)
        return -9;
    memcpy(target, input, size);
    return 0;
}
SceUID ksceKernelRegisterProcEventHandler(const char *name, const SceProcEventHandler *handler,
                                          int unused)
{
    assert(!strcmp(name, "VcoGameSessions") && handler->size == sizeof(*handler) && !unused);
    ++registers;
    events = handler;
    return register_fail ? -20 : 12;
}
int taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *reference, const char *module,
                                   uint32_t library, uint32_t function, const void *callback)
{
    assert(pid == KERNEL_PID);
    if (!strcmp(module, "SceIofilemgr")) {
        assert(library == 0x40FD29C7 && function == 0xC3D34965 && !file_hooks++);
        if (file_hook_fail)
            return -39;
        *reference = 300;
        file_callback = (int (*)(SceUID, const char *, int, SceMode))callback;
        return 35;
    }
    assert(!strcmp(module, "SceKernelModulemgr"));
    if (library == 0xEAED1616) {
        static const uint32_t nids[] = {0xB4C5EF9E, 0x60647592, 0x72CD301F, 0x086867A8,
                                        0x8E4A7716, 0x86EAEA0A, 0x9D674F45, 0x849E78BE};
        unsigned index = module_hook_calls++;
        assert(index < 8 && nids[index] == function);
        if ((int)index == module_fail_index)
            return -31;
        *reference = 200 + index;
        module_callbacks[index] = (int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))callback;
        return 30 + (int)index;
    }
    ++hook_calls;
    assert(hook_calls <= 2);
    if (hook_calls == 1)
        assert(library == 0xC445FA63 && function == 0x432DCC7A);
    else
        assert(library == 0x92C9FFC2 && function == 0x998C7AE9);
    if (hook_both_fail || (hook_first_fail && hook_calls == 1))
        return -30;
    *reference = 123;
    preloaded = (int (*)(SceUID))callback;
    return 14;
}
int fake_game_continue(tai_hook_ref_t reference, ...)
{
    va_list args;
    va_start(args, reference);
    if (reference == 300) {
        (void)va_arg(args, SceUID);
        (void)va_arg(args, const char *);
        (void)va_arg(args, int);
        (void)va_arg(args, SceMode);
        va_end(args);
        assert(syscall_depth == 1 || (file_drive && syscall_depth == 2) ||
               (file_nested && syscall_depth == 3));
        ++file_original_calls;
        return file_original_result;
    }
    if (reference >= 200 && reference < 208) {
        uintptr_t a = va_arg(args, uintptr_t), b = va_arg(args, uintptr_t);
        uintptr_t c = va_arg(args, uintptr_t), d = va_arg(args, uintptr_t);
        va_end(args);
        assert(a == 0x12345678u && b == 0x87654321u && c == 0x40000000u && d == 0xffffffffu);
        assert(!syscall_depth); /* Hook restored user syscall state before SDK body. */
        __atomic_add_fetch(&module_continues, 1, __ATOMIC_RELAXED);
        if (caller == binding.pid && initialized) {
            assert(executor.engine.rows[0].state == VCO_ENTRY_OFF);
            if (module_quiescent_original) {
                assert(executor.stopped && !executor.arena);
                assert(vcoAgentPermission(binding.session, 0) == VCO_AGENT_ARGUMENT);
                return 123;
            }
            int permission;
            do {
                permission = vcoAgentPermission(binding.session, 0);
            } while (permission == VCO_AGENT_BUSY);
            assert(!permission);
            assert(vcoAgentPermission(binding.session, 1) == VCO_AGENT_BUSY);
            if (module_real_concurrent) {
                pthread_mutex_lock(&module_mutex);
                ++module_entered;
                pthread_cond_broadcast(&module_condition);
                while (!(module_release_mask & (1u << (reference - 200))))
                    pthread_cond_wait(&module_condition, &module_mutex);
                pthread_mutex_unlock(&module_mutex);
                return 123;
            }
            bool was_active = module_original_active;
            module_original_active = true;
            if (module_nested) {
                module_nested = false;
                assert(module_callbacks[2](a, b, c, d) == (module_negative ? -90 : 123));
                assert(vcoAgentPermission(binding.session, 1) == VCO_AGENT_BUSY);
            }
            if (module_exit)
                events->exit(binding.pid, NULL, 0);
            module_original_active = was_active;
        }
        return module_negative ? -90 : 123;
    }
    SceUID pid = va_arg(args, SceUID);
    va_end(args);
    assert(reference == 123 && pid > 0);
    ++continues;
    return original_result;
}
int ksceKernelDelayThread(unsigned delay)
{
    assert(delay == 1000 && syscall_depth == 1);
    __atomic_add_fetch(&module_delays, 1, __ATOMIC_RELAXED);
    if (module_gate_fail)
        return -70;
    if (release_loader_gate) {
        release_loader_gate = false;
        vco_service_call_end();
    }
    if (module_real_concurrent) {
        struct timespec pause = {0, 1000000};
        assert(!nanosleep(&pause, NULL));
        return 0;
    }
    clock_us += delay;
    if (module_drive) {
        /* A scheduler switch runs the actual executor + copied syscall path,
         * with the worker's own caller and syscall state. No gate may be held. */
        int32_t old_caller = caller;
        unsigned old_depth = syscall_depth, old_maps = map_count;
        syscall_depth = 0;
        if (module_shutdown_on_delay) {
            module_shutdown_on_delay = false;
            caller = binding.pid;
            assert(!vco_game_executor_stop(&executor));
            VcoAgentResult stopped;
            assert(!vco_game_executor_stopped_result(&executor, &stopped));
            assert(!vcoAgentPublish(user(&stopped, sizeof(stopped)), sizeof(stopped)));
        } else
            execute_game();
        assert(!syscall_depth && !module_original_active);
        caller = old_caller;
        syscall_depth = old_depth;
        map_count = old_maps;
    }
    return 0;
}
SceUID ksceKernelLoadModuleForPid(SceUID pid, const char *path, int flags,
                                  SceKernelLMOption *options)
{
    assert(!strcmp(path, "ux0:vitacheat/VitaCheatOverlayGame.suprx"));
    assert(pid > 0 && !flags && !options);
    ++loads;
    binding.pid = pid; /* File callbacks may run before entry receives its binding. */
    if ((require_loader_context && syscall_depth != 1) ||
        (require_loader_process && (caller != KERNEL_PID || loader_permission != 0x80))) {
        return (int32_t)UINT32_C(0xC0022005);
    }
    if (file_drive) {
        if (file_scope_checks) {
            assert(file_callback(pid + 1, path, SCE_O_RDONLY, 0) == file_original_result);
            assert(file_callback(pid, NULL, SCE_O_RDONLY, 0) == file_original_result);
            assert(file_callback(pid, "ux0:vitacheat/VitaCheatOverlayGame.suprx.extra",
                                 SCE_O_RDONLY, 0) == file_original_result);
            assert(file_callback(pid, "ux0:vitacheat/db/PCSB00031.psv", SCE_O_RDONLY, 0) ==
                   file_original_result);
            assert(file_callback(pid, "ux0:vitacheat/../tai/config.txt", SCE_O_RDONLY, 0) ==
                   file_original_result);
            assert(!file_kernel_calls);
        }
        if (file_flag_checks) {
            const int modes[] = {0,
                                 SCE_O_WRONLY,
                                 SCE_O_RDWR,
                                 SCE_O_RDONLY | SCE_O_CREAT,
                                 SCE_O_RDONLY | SCE_O_APPEND,
                                 SCE_O_RDONLY | SCE_O_TRUNC};
            for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
                assert(file_callback(pid, path, modes[i], 0) == file_original_result);
            assert(!file_kernel_calls);
        }
        int opened = file_callback(pid, path, SCE_O_RDONLY, 0);
        if (opened < 0)
            return (int32_t)UINT32_C(0xC0022005);
    }
    if (load_fail)
        return -40;
    return 15;
}
int ksceKernelStartModuleForPid(SceUID pid, SceUID module, SceSize length, void *args, int flags,
                                SceKernelLMOption *options, int *status)
{
    assert(module == 15 && syscall_depth == 1 && caller == KERNEL_PID);
    assert(length == sizeof(binding) && !flags && !options);
    ++start_calls;
    binding = *(VcoAgentBinding *)args;
    assert(binding.pid == pid && binding.session);
    if (start_sdk_fail)
        return -41;
    int32_t previous = caller;
    int previous_permission = loader_permission;
    unsigned previous_depth = syscall_depth;
    caller = pid;
    /* Module entry runs on its own user thread, with its own syscall state. */
    syscall_depth = 0;
    loader_permission = 0x12;
    /* Actual broker syscall in the module entry proves the loader released
     * its gate. A self-deadlock would return BUSY here. */
    assert(vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)) == VCO_AGENT_OK);
    ++module_starts;
    assert(!syscall_depth);
    syscall_depth = previous_depth;
    loader_permission = previous_permission;
    caller = previous;
    *status = start_fail ? 2 : 0;
    return 0;
}
int ksceKernelStopUnloadModuleForPid(SceUID pid, SceUID module, SceSize length, void *args,
                                     int flags, SceKernelULMOption *options, int *status)
{
    assert(pid == binding.pid && module == 15 && syscall_depth == 1 && caller == KERNEL_PID);
    assert(!length && !args && !flags && !options);
    ++cleanup_calls;
    *status = cleanup_fail ? SCE_KERNEL_STOP_CANCEL : 0;
    return cleanup_fail ? -42 : 0;
}
int ksceIoOpen(const char *path, int flags, SceMode mode)
{
    assert((syscall_depth == 1 || syscall_depth == 2) &&
           !strcmp(path, "ux0:vitacheat/VitaCheatOverlayGame.suprx") && flags == SCE_O_RDONLY &&
           !mode);
    ++file_kernel_calls;
    if (file_nested) {
        unsigned depth = syscall_depth;
        assert(file_callback(binding.pid, path, flags, mode) == file_original_result);
        assert(syscall_depth == depth && file_kernel_calls == 1);
    }
    return file_kernel_result;
}
static void *allocate(void *unused, size_t size)
{
    (void)unused;
    return fail_allocation ? NULL : malloc(size);
}
static void deallocate(void *unused, void *address)
{
    (void)unused;
    free(address);
}
static int alive(void *unused, uint64_t session)
{
    (void)unused;
    assert(!syscall_depth && caller == binding.pid);
    return vcoAgentPermission(session, 1) == VCO_AGENT_OK;
}
static int write_bytes(void *unused, uint64_t session, uint32_t address, const uint8_t *bytes,
                       size_t size)
{
    (void)unused;
    if (!alive(NULL, session) || address < BASE || (uint64_t)address + size > BASE + sizeof(game))
        return -1;
    memcpy(game + (address - BASE), bytes, size);
    return 0;
}
static void initialize_game(void)
{
    assert(!initialized);
    initialized = true;
    VcoAgentAllocator allocator = {NULL, allocate, deallocate};
    VcoCheatMemory memory = {.alive = alive, .write = write_bytes};
    assert(!vco_game_executor_init(&executor, &binding, allocator, memory, NULL, NULL));
}
static VcoServiceStatus status(void)
{
    VcoServiceStatus s;
    caller = shell_pid;
    assert(vcoServiceStatus(user(&s, sizeof(s)), sizeof(s)) == VCO_MAILBOX_OK);
    return s;
}
static void observe(void)
{
    VcoForegroundSample sample = {.abi = VCO_FOREGROUND_PROBE_ABI,
                                  .size = sizeof(sample),
                                  .status = VCO_FOREGROUND_CANDIDATE,
                                  .active_app_id = 10,
                                  .game_app_id = 10,
                                  .active_pid = binding.pid,
                                  .game_pid = binding.pid,
                                  .game_title_id = "PCSB00031"};
    caller = shell_pid;
    assert(vcoServiceObserve(user(&sample, sizeof(sample)), sizeof(sample)) == VCO_MAILBOX_OK);
}
static void setup(void)
{
    assert(!vco_agent_kernel_start());
    assert(!vco_agent_access_start());
    assert(status().state == VCO_SERVICE_NO_GAME);
    assert(!vco_agent_kernel_begin(77, "PCSB00031", &binding));
    caller = 77;
    assert(!vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)));
    initialize_game();
    observe();
    maps[map_count++] = (Mapping){BASE, game, sizeof(game)};
}
static void encode(uint64_t request)
{
    VcoCheatEntry entries[1];
    VcoCode codes[1];
    char names[32];
    VcoCheatFile file = {entries, 1, 0, codes, 1, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    const char text[] = "_V1 Data\n$0200 81000000 00000040\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    assert(!vco_upload_encode(packet, sizeof(packet), request, binding.session, (uint32_t)request,
                              binding.pid, binding.title_id, &file, &packet_length));
}
static void load(void)
{
    caller = shell_pid;
    assert(vcoServiceLoad(user(packet, packet_length), (uint32_t)packet_length) == VCO_MAILBOX_OK);
}
static void poll_game(void)
{
    caller = binding.pid;
    assert(game_poll(binding.session, user(&message, sizeof(message)), sizeof(message),
                     user(payload, sizeof(payload)), sizeof(payload)) == VCO_AGENT_OK);
}
static void execute_game(void)
{
    poll_game();
    clock_us += 1000;
    assert(!vco_game_executor_handle(&executor, &message, payload, message.length,
                                     (uint64_t)clock_us, &result));
    if (message.ticket)
        assert(vcoAgentPublish(user(&result, sizeof(result)), sizeof(result)) == VCO_AGENT_OK);
}
static uint64_t submit(uint64_t request, unsigned operation, bool enabled, bool fail_ticket)
{
    caller = shell_pid;
    VcoControlRequest q = {.request = request,
                           .session = binding.session,
                           .revision = executor.engine.revision,
                           .abi = VCO_CONTROL_ABI,
                           .size = sizeof(q),
                           .operation = operation,
                           .entry = 0,
                           .enabled = enabled,
                           .count = 1};
    uint64_t ticket = 0;
    copy_out_fail = fail_ticket;
    int e = vcoServiceSubmit(user(&q, sizeof(q)), sizeof(q), user(&ticket, sizeof(ticket)));
    assert(e == (fail_ticket ? VCO_MAILBOX_COPY : VCO_MAILBOX_OK));
    if (fail_ticket) {
        struct {
            uint64_t ticket;
            VcoControlRequest request;
        } resume;
        assert(vcoServiceResume(user(&resume, sizeof(resume)), sizeof(resume)) == VCO_MAILBOX_OK);
        assert(!memcmp(&resume.request, &q, sizeof(q)));
        ticket = resume.ticket;
    }
    assert(ticket);
    return ticket;
}
static VcoControlReply receive(uint64_t ticket)
{
    caller = shell_pid;
    VcoControlReply reply;
    assert(vcoServiceReceive(ticket, user(&reply, sizeof(reply)), sizeof(reply)) == VCO_MAILBOX_OK);
    assert(vcoServiceAck(ticket) == VCO_MAILBOX_OK);
    return reply;
}
static void pipeline(void)
{
    setup();
    encode(1);
    load();
    execute_game();
    VcoServiceStatus s = status();
    assert(s.load_request == 1 && s.count == 1 && s.catalog_revision == 1 && !game[0]);
    uint64_t ticket = submit(1, VCO_CONTROL_SET, true, true);
    execute_game();
    assert(game[0] == 0x40);
    caller = shell_pid;
    VcoControlReply reply;
    copy_out_fail = true;
    assert(vcoServiceReceive(ticket, user(&reply, sizeof(reply)), sizeof(reply)) ==
           VCO_MAILBOX_COPY);
    receive(ticket);
    ticket = submit(2, VCO_CONTROL_QUERY, false, false);
    execute_game();
    reply = receive(ticket);
    assert(reply.rows[0].state == VCO_ENTRY_ON && reply.rows[0].acknowledged == 1);
    ticket = submit(3, VCO_CONTROL_SET, false, false);
    execute_game();
    receive(ticket);
    assert(executor.engine.rows[0].state == VCO_ENTRY_OFF);
    caller = 77;
    assert(!vco_game_executor_stop(&executor));
}
static void authorization(void)
{
    setup();
    caller = 77;
    assert(vcoServiceLoad((void *)1, 128) == VCO_MAILBOX_OWNER);
    caller = 99;
    assert(vcoAgentPermission(binding.session, 0) == VCO_AGENT_OWNER);
    VcoAgentBinding forged = binding;
    forged.pid = 88;
    caller = 77;
    assert(vcoAgentAttach(user(&forged, sizeof(forged)), sizeof(forged)) == VCO_AGENT_ARGUMENT);
    assert(vcoAgentPermission(binding.session + 1, 0) == VCO_AGENT_STALE);
}
static void lease(void)
{
    setup();
    caller = 77;
    assert(!vcoAgentPermission(binding.session, 1));
    clock_us += 250000;
    assert(vcoAgentPermission(binding.session, 1) == VCO_AGENT_BUSY);
    assert(!vcoAgentPermission(binding.session, 0));
    observe();
    caller = 77;
    assert(!vcoAgentPermission(binding.session, 1));
    events->stop(77, 1, NULL, 0);
    assert(vcoAgentPermission(binding.session, 1) == VCO_AGENT_BUSY);
    observe();
    caller = 77;
    assert(!vcoAgentPermission(binding.session, 1));
    caller = 99;
    copy_in_fail = true;
    VcoForegroundSample sample = {0};
    assert(vcoServiceObserve(user(&sample, sizeof(sample)), sizeof(sample)) == VCO_MAILBOX_COPY);
    caller = 77;
    assert(vcoAgentPermission(binding.session, 1) == VCO_AGENT_BUSY);
}
static void terminal_test(void)
{
    setup();
    caller = 99;
    assert(!vco_service_call_begin(caller));
    unsigned before = references + copies_in + copies_out;
    assert(!events->kill(77, NULL, 0));
    assert(references + copies_in + copies_out == before);
    vco_service_call_end();
    caller = 77;
    assert(vcoAgentPermission(binding.session, 0) == VCO_AGENT_STALE);
    assert(status().state == VCO_SERVICE_NO_GAME);
    VcoAgentBinding previous = binding;
    assert(!vco_agent_kernel_begin(77, "PCSB00031", &binding));
    assert(binding.session > previous.session);
    caller = 77;
    assert(vcoAgentAttach(user(&previous, sizeof(previous)), sizeof(previous)) == VCO_AGENT_STALE);
    assert(!vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)));
}
static void races(void)
{
    setup();
    title_switch = true;
    observe();
    caller = 77;
    assert(vcoAgentPermission(binding.session, 1) == VCO_AGENT_BUSY);
    observe();
    caller = 77;
    title_exit = true;
    assert(vcoAgentPermission(binding.session, 0) == VCO_AGENT_STALE);
}
static void copies(void)
{
    setup();
    encode(1);
    load();
    caller = 77;
    assert(vcoAgentAttach((void *)0xfffffff0, sizeof(binding)) == VCO_AGENT_ARGUMENT);
    memset(&message, 0x5a, sizeof(message));
    assert(game_poll(binding.session, user(&message, sizeof(message)), sizeof(message), NULL, 0) ==
           VCO_AGENT_LIMIT);
    assert(message.abi == 0x5a5a5a5a);
    copy_out_fail = true;
    assert(game_poll(binding.session, user(&message, sizeof(message)), sizeof(message),
                     user(payload, sizeof(payload)), sizeof(payload)) == VCO_AGENT_COPY);
    execute_game();
    assert(status().load_request == 1);
}
static void processed_failure(void)
{
    setup();
    encode(1);
    load();
    execute_game();
    encode(2);
    fail_allocation = true;
    load();
    execute_game();
    assert(result.error == VCO_E_LIMIT);
    VcoServiceStatus s = status();
    assert(s.load_request == 2 && s.error == VCO_E_LIMIT && s.count == 1);
    fail_allocation = false;
    load();
    poll_game();
    assert(message.operation == VCO_AGENT_IDLE); /* Consumed error receipt is not retried as new. */
    caller = 77;
    assert(!vco_game_executor_stop(&executor));
}
static void owner_restart(bool same_pid)
{
    setup();
    encode(1);
    load();
    execute_game();
    uint64_t ticket = submit(1, VCO_CONTROL_SET, true, false);
    poll_game();
    assert(!vco_game_executor_handle(&executor, &message, payload, message.length,
                                     (uint64_t)clock_us, &result));
    shell_version = 1;
    if (!same_pid)
        shell_pid = 100;
    assert(status().state == VCO_SERVICE_LOADING);
    assert(capture_resets == 1);
    caller = 77;
    assert(vcoAgentPublish(user(&result, sizeof(result)), sizeof(result)) == VCO_AGENT_BUSY);
    assert(vcoAgentPermission(binding.session, 1) == VCO_AGENT_BUSY);
    execute_game();
    assert(message.operation == VCO_AGENT_CLEAR && !executor.engine.bound && !executor.arena);
    caller = shell_pid;
    assert(vcoServiceAck(ticket) == VCO_MAILBOX_TICKET);
    assert(status().count == 0);
    observe();
    caller = 77;
    assert(!vcoAgentPermission(binding.session, 1));
}
static void loader(bool fallback)
{
    hook_first_fail = fallback;
    assert(!vco_agent_loader_start());
    assert(!vco_agent_loader_start());
    assert(registers == 1 && hook_calls == (fallback ? 2u : 1u));
    assert(status().state == VCO_SERVICE_NO_GAME);
    original_result = -50;
    assert(preloaded(77) == -50 && !loads);
    original_result = 42;
    assert(preloaded(99) == 42 && !loads);
    assert(preloaded(77) == 42 && loads == 1 && module_starts == 1);
    assert(status().pid == 77);
}
static void loader_failure(bool entry_failure)
{
    load_fail = !entry_failure;
    start_fail = entry_failure;
    assert(!vco_agent_loader_start());
    status();
    assert(preloaded(77) == original_result && loads == 1);
    VcoServiceStatus s = status();
    assert(s.state == VCO_SERVICE_ERROR && s.error == VCO_E_IO && s.pid == 77);
}
static void loader_binding_busy(bool persistent)
{
    assert(!vco_agent_loader_start());
    assert(status().state == VCO_SERVICE_NO_GAME);
    caller = shell_pid;
    assert(!vco_service_call_begin(caller));
    release_loader_gate = !persistent;
    assert(preloaded(77) == original_result);
    if (persistent) {
        assert(module_delays == 7 && !loads && !module_starts);
        vco_service_call_end();
        assert(status().state == VCO_SERVICE_NO_GAME);
    } else {
        assert(module_delays == 1 && loads == 1 && module_starts == 1);
        assert(status().pid == 77);
    }
    VcoTraceBatch batch;
    assert(!vcoServiceTrace(0, user(&batch, sizeof(batch)), sizeof(batch)));
    bool found = false;
    for (unsigned i = 0; i < batch.count; ++i)
        if (batch.records[i].event == 0xda) {
            assert(batch.records[i].subject == 77);
            assert(batch.records[i].result == (persistent ? VCO_AGENT_BUSY : VCO_AGENT_OK));
            assert(batch.records[i].detail == (persistent ? 8u : 2u));
            found = true;
        }
    assert(found);
}
static void launch_receipt(void)
{
    setup();
    encode(100);
    load();
    execute_game();
    assert(status().load_request == 100);
    VcoAgentBinding old = binding;
    events->exit(binding.pid, NULL, 0);
    assert(!vco_agent_kernel_begin(77, "PCSB00031", &binding));
    assert(binding.session != old.session);
    caller = binding.pid;
    assert(!vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)));
    caller = shell_pid;
    assert(vcoServiceLoad(NULL, 0) == VCO_MAILBOX_OK);
    encode(1);
    load();
    poll_game();
    assert(message.operation == VCO_AGENT_LOAD && message.session == binding.session);
    assert(status().load_request == 0);
}
static void loader_gate(void)
{
    setup();
    caller = 99;
    assert(!vco_service_call_begin(caller));
    vco_agent_kernel_load_result(&binding, -40, -1);
    vco_service_call_end();
    assert(status().state == VCO_SERVICE_ERROR);
}
static void loader_failure_passthrough(bool entry_failure)
{
    loader_failure(entry_failure);
    caller = binding.pid;
    for (unsigned i = 0; i < 8; ++i)
        assert(module_callbacks[i](0x12345678u, 0x87654321u, 0x40000000u, 0xffffffffu) == 123);
    assert(module_continues == 8 && !module_delays);
}
static void loader_context(void)
{
    require_loader_context = true;
    assert(!vco_agent_loader_start());
    status();
    assert(preloaded(77) == original_result && module_starts == 1 && !syscall_depth);
    /* The caller's state must also be restored when the SDK load fails. */
    load_fail = true;
    assert(preloaded(78) == original_result && loads == 2 && !syscall_depth);
    assert(status().state == VCO_SERVICE_ERROR);
}
static void loader_process_context(const char *mode)
{
    require_loader_process = true;
    load_fail = !strcmp(mode, "loader_process_load_failure");
    start_fail = !strcmp(mode, "loader_process_start_failure");
    permission_set_fail = !strcmp(mode, "loader_process_permission_failure");
    pid_set_fail = !strcmp(mode, "loader_process_pid_failure");
    assert(!vco_agent_loader_start());
    status();
    bool already_kernel = !strcmp(mode, "loader_process_already_kernel");
    caller = already_kernel ? KERNEL_PID : 77;
    loader_permission = already_kernel ? 0x80 : 0x12;
    int32_t initial_pid = caller;
    int initial_permission = loader_permission;
    assert(preloaded(77) == original_result);
    assert(caller == initial_pid && loader_permission == initial_permission && !syscall_depth);
    assert(permission_sets == (permission_set_fail ? 1u : 2u));
    assert(pid_sets == (permission_set_fail ? 0u : pid_set_fail ? 1u : 2u));
    assert(loads == (permission_set_fail || pid_set_fail ? 0u : 1u));
    assert(module_starts == (permission_set_fail || pid_set_fail || load_fail ? 0u : 1u));
    VcoServiceStatus s = status();
    bool failed = permission_set_fail || pid_set_fail || load_fail || start_fail;
    assert((s.state == VCO_SERVICE_ERROR) == failed);
    assert(s.pid == 77 && s.error == (failed ? VCO_E_IO : VCO_OK));
}
static void loader_stages(const char *mode)
{
    load_fail = !strcmp(mode, "loader_stages_load_failure");
    start_sdk_fail = !strcmp(mode, "loader_stages_start_failure") ||
                     !strcmp(mode, "loader_stages_cleanup_failure");
    start_fail = !strcmp(mode, "loader_stages_entry_failure");
    cleanup_fail = !strcmp(mode, "loader_stages_cleanup_failure");
    assert(!vco_agent_loader_start());
    status();
    assert(preloaded(77) == original_result);
    assert(caller == shell_pid && loader_permission == 0x12 && !syscall_depth);
    assert(loads == 1 && start_calls == (load_fail ? 0u : 1u));
    assert(cleanup_calls == (start_sdk_fail || start_fail ? 1u : 0u));
    assert(module_starts == (load_fail || start_sdk_fail ? 0u : 1u));
    VcoServiceStatus s = status();
    assert((s.state == VCO_SERVICE_ERROR) == (load_fail || start_sdk_fail || start_fail));
    unsigned counts[3] = {0};
    uint64_t cursor = 0;
    for (unsigned page = 0; page < 8; ++page) {
        VcoTraceBatch batch;
        assert(!vcoServiceTrace(cursor, user(&batch, sizeof(batch)), sizeof(batch)));
        if (!batch.count)
            break;
        for (unsigned i = 0; i < batch.count; ++i) {
            VcoTraceRecord record = batch.records[i];
            cursor = record.sequence;
            if (record.event >= 208 && record.event <= 210) {
                ++counts[record.event - 208];
                assert(record.subject == 77);
                if (record.event == 208)
                    assert(record.result == (load_fail ? -40 : 15) && !record.detail);
                else if (record.event == 209)
                    assert(record.result == (start_sdk_fail ? -41 : 0) &&
                           record.detail == (start_sdk_fail ? UINT32_MAX
                                             : start_fail   ? 2u
                                                            : 0u));
                else
                    assert(record.result == (cleanup_fail ? -42 : 0) &&
                           record.detail == (cleanup_fail ? SCE_KERNEL_STOP_CANCEL : 0u));
            }
        }
    }
    assert(counts[0] == 1 && counts[1] == start_calls && counts[2] == cleanup_calls);
}
static void loader_exports(bool missing)
{
    loader_exports_missing = missing;
    loader_exports_first_missing = !missing;
    if (missing) {
        assert(vco_agent_loader_start() < 0 && !file_hooks && !module_hook_calls && !hook_calls);
        assert(vco_agent_loader_start() < 0 && !loads && !start_calls);
        assert(status().state == VCO_SERVICE_ERROR);
    } else {
        assert(!vco_agent_loader_start());
        status();
        assert(preloaded(77) == original_result && loads == 1 && start_calls == 1);
        assert(!syscall_depth && caller == shell_pid && loader_permission == 0x12);
    }
}
static void loader_file(const char *mode)
{
    file_hook_fail = !strcmp(mode, "loader_file_hook_failure");
    file_drive = true;
    file_scope_checks = !strcmp(mode, "loader_file_scope");
    file_flag_checks = !strcmp(mode, "loader_file_flags");
    file_nested = !strcmp(mode, "loader_file_nested");
    if (!strcmp(mode, "loader_file_original"))
        file_original_result = 63;
    if (!strcmp(mode, "loader_file_failure"))
        file_kernel_result = -44;
    if (file_hook_fail) {
        assert(vco_agent_loader_start() == -39 && !module_hook_calls && !hook_calls);
        assert(vco_agent_loader_start() == -39 && file_hooks == 1);
        assert(status().state == VCO_SERVICE_ERROR);
        return;
    }
    assert(!vco_agent_loader_start() && file_hooks == 1);
    status();
    assert(preloaded(77) == original_result && loads == 1);
    assert(file_kernel_calls == (file_original_result < 0 ? 1u : 0u));
    assert(!syscall_depth);
    caller = binding.pid;
    if (file_kernel_result < 0) {
        assert(status().state == VCO_SERVICE_ERROR);
        assert(module_callbacks[0](0x12345678u, 0x87654321u, 0x40000000u, 0xffffffffu) == 123);
    } else
        assert(module_starts == 1);
    unsigned before = file_kernel_calls;
    assert(file_callback(77, "ux0:vitacheat/VitaCheatOverlayGame.suprx", SCE_O_RDONLY, 0) ==
           file_original_result);
    assert(file_kernel_calls == before); /* Scope ends even on load failure. */
    if (!strcmp(mode, "loader_file_retry")) {
        load_fail = true;
        assert(preloaded(77) == original_result);
        assert(file_callback(77, "ux0:vitacheat/VitaCheatOverlayGame.suprx", SCE_O_RDONLY, 0) ==
               file_original_result);
        assert(file_kernel_calls == before + 1);
    }
}
static void stale_failure(void)
{
    setup();
    VcoAgentBinding previous = binding;
    assert(!vco_agent_kernel_begin(78, "PCSB00031", &binding));
    caller = 78;
    assert(!vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)));
    vco_agent_kernel_load_result(&previous, -40, -1);
    assert(status().state != VCO_SERVICE_ERROR && status().pid == 78);
}
static void game_fault(bool replacement)
{
    setup();
    caller = shell_pid;
    assert(vcoAgentFault(binding.session, VCO_E_IO) == VCO_AGENT_OWNER);
    caller = binding.pid;
    assert(vcoAgentFault(binding.session, 0) == VCO_AGENT_ARGUMENT);
    assert(vcoAgentFault(binding.session, VCO_E_METADATA + 1) == VCO_AGENT_ARGUMENT);
    assert(!vcoAgentFault(binding.session + 20, VCO_E_IO));
    assert(status().state != VCO_SERVICE_ERROR); /* forged/old session ignored */
    caller = binding.pid;
    uint64_t previous = binding.session;
    assert(!vcoAgentFault(previous, VCO_E_RESTORE));
    assert(vcoAgentFault(previous, VCO_E_IO) == VCO_AGENT_BUSY);
    if (replacement) {
        assert(!vco_agent_kernel_begin(binding.pid, binding.title_id, &binding));
        assert(binding.session != previous);
        assert(!vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)));
        assert(status().state != VCO_SERVICE_ERROR);
    } else {
        vco_agent_kernel_load_result(&binding, -40, -1);
        VcoServiceStatus s = status();
        assert(s.state == VCO_SERVICE_ERROR && s.error == VCO_E_RESTORE && s.pid == binding.pid);
        caller = binding.pid;
        assert(vcoAgentPermission(binding.session, 0) == VCO_AGENT_ARGUMENT);
    }
}
static void startup_failure(unsigned kind)
{
    title_missing = kind == 1;
    register_fail = kind == 2;
    hook_both_fail = kind == 3;
    assert(vco_agent_loader_start() < 0);
    unsigned called = registers + hook_calls;
    assert(vco_agent_loader_start() < 0 && called == registers + hook_calls);
    assert(status().state == VCO_SERVICE_ERROR);
    if (kind == 3)
        assert(hook_calls == 2);
}
typedef struct {
    void *message, *payload, *status, *observation;
} ConcurrentBuffers;
static void *game_thread(void *context)
{
    ConcurrentBuffers *buffers = context;
    caller = 77;
    for (unsigned i = 0; i < 500; ++i) {
        int e = game_poll(binding.session, buffers->message, sizeof(message), buffers->payload,
                          sizeof(payload));
        assert(e == VCO_AGENT_OK || e == VCO_AGENT_BUSY);
    }
    return NULL;
}
static void *shell_thread(void *context)
{
    ConcurrentBuffers *buffers = context;
    caller = 99;
    for (unsigned i = 0; i < 500; ++i) {
        int e = vcoServiceStatus(buffers->status, sizeof(VcoServiceStatus));
        assert(e == VCO_MAILBOX_OK || e == VCO_MAILBOX_BUSY);
        if (i == 250)
            events->stop(77, 1, NULL, 0);
        e = vcoServiceObserve(buffers->observation, sizeof(VcoForegroundSample));
        assert(e == VCO_MAILBOX_OK || e == VCO_MAILBOX_BUSY);
    }
    return NULL;
}
static void concurrent(void)
{
    setup();
    VcoServiceStatus s;
    VcoForegroundSample sample = {.abi = VCO_FOREGROUND_PROBE_ABI,
                                  .size = sizeof(sample),
                                  .status = VCO_FOREGROUND_CANDIDATE,
                                  .active_app_id = 10,
                                  .game_app_id = 10,
                                  .active_pid = 77,
                                  .game_pid = 77,
                                  .game_title_id = "PCSB00031"};
    ConcurrentBuffers buffers = {user(&message, sizeof(message)), user(payload, sizeof(payload)),
                                 user(&s, sizeof(s)), user(&sample, sizeof(sample))};
    pthread_t g, ui;
    assert(!pthread_create(&g, NULL, game_thread, &buffers));
    assert(!pthread_create(&ui, NULL, shell_thread, &buffers));
    assert(!pthread_join(g, NULL));
    assert(!pthread_join(ui, NULL));
    assert(references == releases + 1); /* Only the retained Shell reference remains. */
}
static void access_test(void)
{
    setup();
    caller = 77;
    uint8_t bytes[4] = {1, 2, 3, 4}, out[4] = {0};
    void *input = user(bytes, sizeof(bytes)), *output = user(out, sizeof(out));
    for (unsigned width = 1; width <= 4; width *= 2) {
        assert(!game_write(binding.session, BASE + 1, input, width));
        assert(!game_read(binding.session, BASE + 1, output, width));
        assert(!memcmp(out, bytes, width));
    }
    caller = 99;
    assert(game_write(binding.session, BASE, input, 4) == VCO_AGENT_OWNER);
    caller = 77;
    assert(game_read(binding.session + 1, BASE, output, 4) == VCO_AGENT_STALE);
    clock_us += 250000;
    assert(game_write(binding.session, BASE, input, 4) == VCO_AGENT_BUSY);
}
static void access_faults(void)
{
    setup();
    caller = 77;
    uint8_t bytes[4] = {1, 2, 3, 4}, out[4] = {9, 9, 9, 9};
    void *input = user(bytes, 4), *output = user(out, 4);
    assert(game_read(binding.session, BASE, output, 3) == VCO_AGENT_ARGUMENT);
    assert(game_read(binding.session, 0x10000000, output, 4) == VCO_AGENT_ARGUMENT);
    assert(game_read(binding.session, UINT32_MAX - 1, output, 4) == VCO_AGENT_ARGUMENT);
    assert(game_write(binding.session, BASE, (void *)0x1000, 4) == VCO_AGENT_ARGUMENT);
    copy_in_fail = true;
    assert(game_read(binding.session, BASE, output, 4) == VCO_AGENT_COPY);
    copy_data_in_fail = true;
    assert(game_read(binding.session, BASE, output, 4) == VCO_AGENT_IO);
    assert(out[0] == 9);
    copy_in_fail = true;
    assert(game_write(binding.session, BASE, input, 4) == VCO_AGENT_COPY);
    assert(game[0] == 0);
    copy_out_fail = true;
    assert(game_write(binding.session, BASE, input, 4) == VCO_AGENT_IO);
    assert(game[0] == 0);
    copy_out_fail = true;
    assert(game_read(binding.session, BASE, output, 4) == VCO_AGENT_COPY);
    assert(out[0] == 9);
    assert(game_read(binding.session, BASE + 32, output, 4) == VCO_AGENT_IO);
}
static void syscall_packets(const char *mode)
{
    setup();
    caller = 77;
    VcoAgentMessage message_out;
    VcoGameModuleInfo info = {.uid = 999};
    uint8_t bytes[4] = {9, 9, 9, 9};
    VcoAgentPollRequest p = {.session = binding.session,
                             .abi = VCO_AGENT_ABI,
                             .message = (uintptr_t)user(&message_out, sizeof(message_out)),
                             .payload = (uintptr_t)user(payload, sizeof(payload)),
                             .message_length = sizeof(message_out),
                             .capacity = sizeof(payload)};
    VcoAgentMemoryRequest m = {.session = binding.session,
                               .abi = VCO_AGENT_ABI,
                               .address = BASE,
                               .buffer = (uintptr_t)user(bytes, sizeof(bytes)),
                               .width = 4};
    VcoAgentModuleRequest n = {.session = binding.session,
                               .abi = VCO_AGENT_ABI,
                               .uid = 11,
                               .output = (uintptr_t)user(&info, sizeof(info)),
                               .length = sizeof(info)};
    void *pp = user(&p, sizeof(p)), *mp = user(&m, sizeof(m)), *np = user(&n, sizeof(n));
    if (!strcmp(mode, "syscall_packets_headers")) {
        assert(vcoAgentPoll(pp, sizeof(p) - 1) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentRead(mp, sizeof(m) + 1) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentWrite(mp, 0) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentModuleInfo(np, sizeof(n) - 1) == VCO_AGENT_ARGUMENT);
        for (unsigned i = 0; i < 2; ++i) {
            p.abi = m.abi = n.abi = i ? VCO_AGENT_ABI : 0x10003u;
            p.reserved = m.reserved = n.reserved = i;
            assert(vcoAgentPoll(pp, sizeof(p)) == VCO_AGENT_ARGUMENT);
            assert(vcoAgentRead(mp, sizeof(m)) == VCO_AGENT_ARGUMENT);
            assert(vcoAgentWrite(mp, sizeof(m)) == VCO_AGENT_ARGUMENT);
            assert(vcoAgentModuleInfo(np, sizeof(n)) == VCO_AGENT_ARGUMENT);
        }
    } else if (!strcmp(mode, "syscall_packets_addresses")) {
        p.message = m.buffer = n.output = UINT64_C(0x150000000);
        assert(vcoAgentPoll(pp, sizeof(p)) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentRead(mp, sizeof(m)) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentWrite(mp, sizeof(m)) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentModuleInfo(np, sizeof(n)) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentPoll((void *)0x2000, sizeof(p)) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentRead(NULL, sizeof(m)) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentWrite((void *)0xfffffff0u, sizeof(m)) == VCO_AGENT_ARGUMENT);
        assert(vcoAgentModuleInfo(NULL, sizeof(n)) == VCO_AGENT_ARGUMENT);
    } else if (!strcmp(mode, "syscall_packets_copies")) {
        copy_in_fail = true;
        assert(vcoAgentPoll(pp, sizeof(p)) == VCO_AGENT_COPY);
        copy_in_fail = true;
        assert(vcoAgentRead(mp, sizeof(m)) == VCO_AGENT_COPY);
        copy_in_fail = true;
        assert(vcoAgentWrite(mp, sizeof(m)) == VCO_AGENT_COPY);
        copy_in_fail = true;
        assert(vcoAgentModuleInfo(np, sizeof(n)) == VCO_AGENT_COPY);
    } else if (!strcmp(mode, "syscall_packets_session")) {
        p.session = m.session = n.session = binding.session | UINT64_C(0x100000000);
        assert(vcoAgentPoll(pp, sizeof(p)) == VCO_AGENT_STALE);
        assert(vcoAgentRead(mp, sizeof(m)) == VCO_AGENT_STALE);
        assert(vcoAgentWrite(mp, sizeof(m)) == VCO_AGENT_STALE);
        assert(vcoAgentModuleInfo(np, sizeof(n)) == VCO_AGENT_STALE);
    } else {
        assert(!strcmp(mode, "syscall_packets_snapshot"));
        mutate_request = true;
        assert(!vcoAgentWrite(mp, sizeof(m)) && !mutate_request);
        assert(!memcmp(game, bytes, 4) && m.abi == 0xccccccccu);
        assert(!syscall_depth);
        return;
    }
    assert(bytes[0] == 9 && game[0] == 0 && info.uid == 999 && !syscall_depth);
    /* A rejected packet does not leave the broker gate owned. */
    assert(!vcoAgentPermission(binding.session, 1));
}
static void access_races(void)
{
    setup();
    caller = 77;
    uint8_t bytes[4] = {1, 2, 3, 4};
    copy_switch = true;
    assert(game_write(binding.session, BASE, user(bytes, 4), 4) == VCO_AGENT_BUSY);
    assert(!game[0]);
    observe();
    caller = 77;
    VcoGameModuleInfo info = {.uid = 999};
    info_exit = true;
    assert(game_info(binding.session, 11, user(&info, sizeof(info)), sizeof(info)) ==
           VCO_AGENT_STALE);
    assert(info.uid == 999);
}
static void modules_test(void)
{
    setup();
    caller = 77;
    VcoGameModuleList list = {0};
    void *out = user(&list, sizeof(list));
    assert(!vcoAgentModules(binding.session, out, sizeof(list)));
    assert(list.count == 2 && list.ids[0] == 11 && list.ids[1] == 12);
    native_module_count = 256;
    assert(!vcoAgentModules(binding.session, out, sizeof(list)) && list.count == 256);
    native_module_count = 257;
    assert(vcoAgentModules(binding.session, out, sizeof(list)) == VCO_AGENT_LIMIT);
    assert(list.count == 256);
    native_module_count = 2;
    duplicate_module = true;
    assert(vcoAgentModules(binding.session, out, sizeof(list)) == VCO_AGENT_IO);
    duplicate_module = false;
    modules_fail = true;
    assert(vcoAgentModules(binding.session, out, sizeof(list)) == VCO_AGENT_IO);
}
static void module_fields(void)
{
    setup();
    caller = 77;
    VcoGameModuleInfo info;
    void *out = user(&info, sizeof(info));
    assert(!game_info(binding.session, 11, out, sizeof(info)));
    assert(info.uid == 11 && info.process_uid == 111 && info.segments[0].base == BASE &&
           info.segments[0].size == 8 && info.segments[0].permissions == 5);
    unsigned calls = info_calls;
    assert(game_info(binding.session, 1234, out, sizeof(info)) == VCO_AGENT_ARGUMENT);
    assert(info_calls == calls && info.uid == 11); /* Foreign GUID never reaches Info. */
    empty_segment = true;
    assert(!game_info(binding.session, 11, out, sizeof(info)));
    assert(info.segments[0].base == BASE && !info.segments[0].size);
    empty_segment = false;
    bad_segment = true;
    assert(game_info(binding.session, 11, out, sizeof(info)) == VCO_AGENT_IO);
    bad_segment = false;
    invalid_module_name = true;
    assert(game_info(binding.session, 11, out, sizeof(info)) == VCO_AGENT_IO);
    invalid_module_name = false;
    info_fail = true;
    assert(game_info(binding.session, 11, out, sizeof(info)) == VCO_AGENT_IO);
}
static void pad_test(void)
{
    setup();
    caller = 77;
    VcoGamePadSample sample;
    void *out = user(&sample, sizeof(sample));
    pad_buttons = SCE_CTRL_SQUARE;
    assert(!vcoAgentPad(binding.session, out, sizeof(sample)));
    assert(sample.device == SCE_CTRL_TYPE_PHY && sample.buttons == SCE_CTRL_SQUARE &&
           !sample.capturing && sample.stamp == (uint64_t)clock_us);
    capturing = 1;
    capture_epoch = 7;
    assert(!vcoAgentPad(binding.session, out, sizeof(sample)));
    assert(sample.capturing == 1 && sample.capture_epoch == 7);
    topology_change = true;
    pad_calls = 0;
    assert(vcoAgentPad(binding.session, out, sizeof(sample)) == VCO_AGENT_IO);
}
static void access_startup(bool fallback)
{
    access_first_missing = fallback;
    access_missing = !fallback;
    if (fallback) {
        setup();
        caller = 77;
        VcoGameModuleList list;
        assert(!vcoAgentModules(binding.session, user(&list, sizeof(list)), sizeof(list)));
        assert(list.count == 2);
    } else {
        assert(vco_agent_loader_start() < 0);
        assert(vco_agent_access_start() < 0 && !hook_calls);
        assert(status().state == VCO_SERVICE_ERROR);
    }
}
static int call_module(unsigned index)
{
    caller = binding.pid;
    return module_callbacks[index](0x12345678u, 0x87654321u, 0x40000000u, 0xffffffffu);
}
static VcoError module_restore_failure(void *unused)
{
    (void)unused;
    return VCO_E_RESTORE;
}
static void module_boundary_test(const char *mode)
{
    assert(!vco_agent_loader_start());
    assert(module_hook_calls == 8 && hook_calls == 1);
    setup();
    encode(1);
    load();
    execute_game();
    uint64_t ticket = submit(1, VCO_CONTROL_SET, true, false);
    execute_game();
    caller = shell_pid;
    assert(!vcoServiceAck(ticket));
    assert(executor.engine.rows[0].state == VCO_ENTRY_ON && game[0] == 0x40);
    uint64_t old_revision = executor.engine.revision;
    if (!strcmp(mode, "module_restore_failure")) {
        executor.finalize = module_restore_failure;
        module_drive = true;
        assert(call_module(5) == (int32_t)UINT32_C(0x8002D01D));
        assert(!module_continues && executor.engine.blocked_restore);
        assert(status().state == VCO_SERVICE_ERROR && status().error == VCO_E_RESTORE);
        assert(call_module(5) == (int32_t)UINT32_C(0x8002D01D));
        assert(!module_continues);
        return;
    }
    if (!strcmp(mode, "module_gate_failure")) {
        caller = 77;
        VcoModuleCall call;
        module_drive = true;
        assert(!vco_agent_kernel_module_begin(&call));
        assert(call.claim);
        caller = 99;
        assert(!vco_service_call_begin(caller));
        caller = 77;
        module_gate_fail = true;
        assert(vco_agent_kernel_module_end(&call) == VCO_AGENT_BUSY);
        caller = 99;
        vco_service_call_end();
        assert(status().state == VCO_SERVICE_ERROR && status().error == VCO_E_IO);
        return;
    }
    module_negative = !strcmp(mode, "module_negative");
    module_nested = !strcmp(mode, "module_nested");
    module_exit = !strcmp(mode, "module_exit");
    module_drive = strcmp(mode, "module_timeout") != 0;
    if (!module_drive) {
        assert(call_module(5) == (int32_t)UINT32_C(0x8002D01D));
        assert(module_delays > 0 && module_delays <= 500 && !module_continues);
        execute_game(); /* Abandoned SDK call still receives all-OFF cleanup. */
        assert(executor.engine.revision > old_revision &&
               executor.engine.rows[0].state == VCO_ENTRY_OFF);
        observe();
        caller = 77;
        assert(!vcoAgentPermission(binding.session, 1));
    } else {
        assert(call_module(5) == (module_negative ? -90 : 123));
        assert(module_continues == (!strcmp(mode, "module_nested") ? 2u : 1u));
        assert(executor.engine.revision == old_revision + 1);
        assert(executor.engine.rows[0].state == VCO_ENTRY_OFF);
        if (module_exit)
            assert(status().state == VCO_SERVICE_NO_GAME);
        else {
            caller = 77;
            assert(vcoAgentPermission(binding.session, 1) == VCO_AGENT_BUSY);
            observe();
            caller = 77;
            assert(!vcoAgentPermission(binding.session, 1));
        }
    }
    if (!module_exit) {
        module_drive = true;
        /* Every export passes all argument words unchanged; lists stay OFF. */
        for (unsigned i = 0; i < 8; ++i) {
            assert(call_module(i) == (module_negative ? -90 : 123));
            observe();
        }
        assert(executor.engine.rows[0].state == VCO_ENTRY_OFF);
    }
}
static void module_hook_failure(unsigned index)
{
    module_fail_index = (int)index;
    assert(vco_agent_loader_start() == -31 && !hook_calls);
    assert(module_hook_calls == index + 1);
    assert(status().state == VCO_SERVICE_ERROR);
    caller = 1234;
    for (unsigned i = 0; i < index; ++i)
        assert(module_callbacks[i](0x12345678u, 0x87654321u, 0x40000000u, 0xffffffffu) == 123);
    assert(!module_delays);
}
typedef struct {
    VcoAgentMessage message;
    VcoAgentResult result;
    uint8_t payload[VCO_UPLOAD_MAX];
    void *user_message, *user_result, *user_payload;
} ModuleWorkerBuffers;
static void *module_actual_worker(void *context)
{
    ModuleWorkerBuffers *buffers = context;
    caller = 77;
    while (!__atomic_load_n(&module_worker_stop, __ATOMIC_ACQUIRE)) {
        int e = game_poll(binding.session, buffers->user_message, sizeof(buffers->message),
                          buffers->user_payload, sizeof(buffers->payload));
        assert(e == VCO_AGENT_OK || e == VCO_AGENT_BUSY);
        if (!e) {
            assert(!vco_game_executor_handle(
                &executor, &buffers->message, buffers->payload, buffers->message.length,
                (uint64_t)ksceKernelGetSystemTimeWide(), &buffers->result));
            if (buffers->message.ticket) {
                e = vcoAgentPublish(buffers->user_result, sizeof(buffers->result));
                assert(e == VCO_AGENT_OK || e == VCO_AGENT_BUSY);
            }
        }
        struct timespec pause = {0, 1000000};
        assert(!nanosleep(&pause, NULL));
    }
    return NULL;
}
static void *module_actual_caller(void *context)
{
    assert(call_module((unsigned)(uintptr_t)context) == 123);
    return NULL;
}
static void module_concurrent(void)
{
    module_real_concurrent = true;
    assert(!vco_agent_loader_start());
    setup();
    encode(1);
    load();
    execute_game();
    uint64_t ticket = submit(1, VCO_CONTROL_SET, true, false);
    execute_game();
    caller = 99;
    assert(!vcoServiceAck(ticket));
    assert(executor.engine.rows[0].state == VCO_ENTRY_ON && game[0] == 0x40);
    uint64_t revision = executor.engine.revision;
    ModuleWorkerBuffers *buffers = calloc(1, sizeof(*buffers));
    assert(buffers);
    buffers->user_message = user(&buffers->message, sizeof(buffers->message));
    buffers->user_result = user(&buffers->result, sizeof(buffers->result));
    buffers->user_payload = user(buffers->payload, sizeof(buffers->payload));
    pthread_t worker, first, second;
    assert(!pthread_create(&worker, NULL, module_actual_worker, buffers));
    assert(!pthread_create(&first, NULL, module_actual_caller, (void *)(uintptr_t)0));
    assert(!pthread_create(&second, NULL, module_actual_caller, (void *)(uintptr_t)1));
    pthread_mutex_lock(&module_mutex);
    while (module_entered < 2)
        pthread_cond_wait(&module_condition, &module_mutex);
    module_release_mask = 1;
    pthread_cond_broadcast(&module_condition);
    pthread_mutex_unlock(&module_mutex);
    assert(!pthread_join(first, NULL));
    caller = 77;
    int e = vcoAgentPermission(binding.session, 1);
    assert(e == VCO_AGENT_BUSY); /* Second original call still owns a claim. */
    pthread_mutex_lock(&module_mutex);
    module_release_mask = 3;
    pthread_cond_broadcast(&module_condition);
    pthread_mutex_unlock(&module_mutex);
    assert(!pthread_join(second, NULL));
    __atomic_store_n(&module_worker_stop, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(worker, NULL));
    assert(executor.engine.revision == revision + 1 && module_continues == 2);
    assert(executor.engine.rows[0].state == VCO_ENTRY_OFF);
    observe();
    caller = 77;
    assert(!vcoAgentPermission(binding.session, 1));
    free(buffers);
}
static void stopped_native(const char *mode)
{
    assert(!vco_agent_loader_start());
    setup();
    encode(1);
    load();
    execute_game();
    if (!strcmp(mode, "stopped_module_race")) {
        module_drive = module_shutdown_on_delay = module_quiescent_original = true;
        assert(call_module(5) == 123 && module_continues == 1);
        assert(executor.stopped);
        unsigned old_delays = module_delays;
        assert(call_module(4) == 123 && module_delays == old_delays);
        return;
    }
    caller = 77;
    assert(!vco_game_executor_stop(&executor));
    VcoAgentResult stopped;
    assert(!vco_game_executor_stopped_result(&executor, &stopped));
    void *address = user(&stopped, sizeof(stopped));
    if (!strcmp(mode, "stopped_stale")) {
        assert(!vco_agent_kernel_begin(77, binding.title_id, &binding));
        assert(!vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)));
        assert(vcoAgentPublish(address, sizeof(stopped)) == VCO_AGENT_STALE);
        assert(!vcoAgentPermission(binding.session, 0));
        return;
    }
    if (!strcmp(mode, "stopped_busy")) {
        caller = 99;
        assert(!vco_service_call_begin(caller));
        caller = 77;
        assert(vcoAgentPublish(address, sizeof(stopped)) == VCO_AGENT_BUSY);
        vco_service_call_end();
    } else if (!strcmp(mode, "stopped_copy")) {
        copy_in_fail = true;
        assert(vcoAgentPublish(address, sizeof(stopped)) == VCO_AGENT_COPY);
    }
    assert(!vcoAgentPublish(address, sizeof(stopped)));
    assert(!vcoAgentPublish(address, sizeof(stopped)));
    assert(vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)) == VCO_AGENT_CLOSED);
    assert(status().state == VCO_SERVICE_ERROR && status().error == VCO_E_SESSION);
    module_quiescent_original = true;
    unsigned old_delays = module_delays;
    assert(call_module(3) == 123 && call_module(4) == 123 && module_delays == old_delays);
}
static void game_diagnostics(const char *mode)
{
    setup();
    VcoServiceStatus before = status();
    caller = binding.pid;
    VcoAgentTraceRequest request = {.session = binding.session,
                                    .abi = VCO_AGENT_ABI,
                                    .size = sizeof(request),
                                    .event = VCO_GAME_TRACE_INJECT,
                                    .result = -20,
                                    .subject = BASE + 0x13971e,
                                    .detail = 4};
    void *address = user(&request, sizeof(request));
    if (!strcmp(mode, "trace_packet")) {
        assert(vcoAgentTrace(address, sizeof(request) - 1) == VCO_AGENT_ARGUMENT);
        request.size--;
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_ARGUMENT);
        request.size++;
        request.abi--;
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_ARGUMENT);
        request.abi++;
        request.event = VCO_GAME_TRACE_INJECT - 1;
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_ARGUMENT);
        request.event = VCO_GAME_TRACE_QUERY + 1;
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_ARGUMENT);
        request.event = VCO_GAME_TRACE_INJECT;
    } else if (!strcmp(mode, "trace_authorization")) {
        caller = shell_pid;
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_OWNER);
        caller = binding.pid;
        request.session++;
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_STALE);
        request.session--;
    } else if (!strcmp(mode, "trace_copy")) {
        copy_in_fail = true;
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_COPY);
    } else {
        assert(!strcmp(mode, "trace_limit"));
        for (unsigned i = 0; i < 64; ++i)
            assert(!vcoAgentTrace(address, sizeof(request)));
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_LIMIT);
        assert(!vco_agent_kernel_begin(binding.pid, binding.title_id, &binding));
        request.session = binding.session;
        assert(vcoAgentTrace(address, sizeof(request)) == VCO_AGENT_ARGUMENT);
        assert(!vcoAgentAttach(user(&binding, sizeof(binding)), sizeof(binding)));
        assert(!vcoAgentTrace(address, sizeof(request)));
        return;
    }
    assert(!vcoAgentTrace(address, sizeof(request)));
    VcoServiceStatus after = status();
    assert(!memcmp(&before, &after, sizeof(before)));
    VcoTraceBatch batch;
    caller = shell_pid;
    assert(!vcoServiceTrace(0, user(&batch, sizeof(batch)), sizeof(batch)));
    bool found = false;
    for (unsigned i = 0; i < batch.count; ++i)
        if (batch.records[i].event == VCO_GAME_TRACE_INJECT) {
            assert(batch.records[i].result == -20 && batch.records[i].subject == request.subject &&
                   batch.records[i].detail == request.detail);
            found = true;
        }
    assert(found);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "loader_binding_retry"))
        loader_binding_busy(false);
    else if (!strcmp(argv[1], "loader_binding_exhaustion"))
        loader_binding_busy(true);
    else if (!strcmp(argv[1], "launch_receipt"))
        launch_receipt();
    else if (!strcmp(argv[1], "pipeline"))
        pipeline();
    else if (!strcmp(argv[1], "authorization"))
        authorization();
    else if (!strcmp(argv[1], "lease"))
        lease();
    else if (!strcmp(argv[1], "terminal"))
        terminal_test();
    else if (!strcmp(argv[1], "races"))
        races();
    else if (!strcmp(argv[1], "copies"))
        copies();
    else if (!strcmp(argv[1], "processed_failure"))
        processed_failure();
    else if (!strcmp(argv[1], "owner_restart"))
        owner_restart(false);
    else if (!strcmp(argv[1], "owner_same_pid"))
        owner_restart(true);
    else if (!strcmp(argv[1], "loader"))
        loader(false);
    else if (!strcmp(argv[1], "loader_fallback"))
        loader(true);
    else if (!strcmp(argv[1], "loader_failure"))
        loader_failure(false);
    else if (!strcmp(argv[1], "loader_start_failure"))
        loader_failure(true);
    else if (!strcmp(argv[1], "loader_context"))
        loader_context();
    else if (!strncmp(argv[1], "loader_process_", 15))
        loader_process_context(argv[1]);
    else if (!strncmp(argv[1], "loader_stages_", 14))
        loader_stages(argv[1]);
    else if (!strcmp(argv[1], "loader_exports_missing"))
        loader_exports(true);
    else if (!strcmp(argv[1], "loader_exports_fallback"))
        loader_exports(false);
    else if (!strcmp(argv[1], "loader_failure_passthrough"))
        loader_failure_passthrough(false);
    else if (!strcmp(argv[1], "loader_start_failure_passthrough"))
        loader_failure_passthrough(true);
    else if (!strncmp(argv[1], "loader_file_", 12))
        loader_file(argv[1]);
    else if (!strcmp(argv[1], "loader_gate"))
        loader_gate();
    else if (!strcmp(argv[1], "stale_failure"))
        stale_failure();
    else if (!strcmp(argv[1], "game_fault"))
        game_fault(false);
    else if (!strcmp(argv[1], "stale_game_fault"))
        game_fault(true);
    else if (!strcmp(argv[1], "missing_title"))
        startup_failure(1);
    else if (!strcmp(argv[1], "register_failure"))
        startup_failure(2);
    else if (!strcmp(argv[1], "hook_failure"))
        startup_failure(3);
    else if (!strcmp(argv[1], "concurrent"))
        concurrent();
    else if (!strcmp(argv[1], "access"))
        access_test();
    else if (!strcmp(argv[1], "access_faults"))
        access_faults();
    else if (!strncmp(argv[1], "syscall_packets_", 16))
        syscall_packets(argv[1]);
    else if (!strncmp(argv[1], "trace_", 6))
        game_diagnostics(argv[1]);
    else if (!strcmp(argv[1], "access_races"))
        access_races();
    else if (!strcmp(argv[1], "modules"))
        modules_test();
    else if (!strcmp(argv[1], "module_fields"))
        module_fields();
    else if (!strcmp(argv[1], "pad"))
        pad_test();
    else if (!strcmp(argv[1], "access_missing"))
        access_startup(false);
    else if (!strcmp(argv[1], "access_fallback"))
        access_startup(true);
    else if (!strncmp(argv[1], "module_hook_fail_", 17))
        module_hook_failure((unsigned)strtoul(argv[1] + 17, NULL, 10));
    else if (!strcmp(argv[1], "module_concurrent"))
        module_concurrent();
    else if (!strncmp(argv[1], "stopped_", 8))
        stopped_native(argv[1]);
    else if (!strncmp(argv[1], "module_", 7))
        module_boundary_test(argv[1]);
    else
        assert(!"unknown case");
    assert(!syscall_depth);
    if (executor.arena)
        deallocate(NULL, executor.arena);
    printf("game kernel %s passed\n", argv[1]);
    return 0;
}
