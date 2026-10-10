#include "agent/vita_memory.h"
#include "agent/native_service.h"
#include <taihen.h>
#include <psp2/kernel/threadmgr.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BASE UINT32_C(0x81000000)
#define DATA UINT32_C(0x81800000)
#define HEAP UINT32_C(0x83000000)
static VcoGameNativeMemory native;
static VcoAgentBinding binding = {.session = 1,
                                  .abi = VCO_AGENT_ABI,
                                  .size = sizeof(binding),
                                  .pid = 77,
                                  .title_id = "PCSB00031"};
static VcoCheatMemory memory;
static bool live = true, foreground = true, read_fail, write_fail, list_fail, info_fail;
static bool inject_fail, release_fail, change_on_inject, background_on_inject, change_list;
static unsigned reads, writes, injections, abs_injections, releases, lists, infos;
static unsigned fail_after;
static int release_success;
static VcoAgentTraceRequest diagnostics[128];
static unsigned permission_busy, list_busy, info_busy, read_busy, diagnostic_busy, delays;
static bool revoke_on_delay;
static unsigned diagnostic_count;
static int diagnostic_result;
int vcoAgentTrace(const VcoAgentTraceRequest *request, uint32_t length)
{
    assert(length == sizeof(*request) && request->size == length && request->abi == VCO_AGENT_ABI &&
           request->session == binding.session);
    assert(diagnostic_count < 128);
    diagnostics[diagnostic_count++] = *request;
    if (diagnostic_busy) {
        --diagnostic_busy;
        return VCO_AGENT_BUSY;
    }
    return diagnostic_result;
}
int sceKernelDelayThread(SceUInt delay)
{
    assert(delay == 1000);
    ++delays;
    if (revoke_on_delay)
        live = false;
    return 0;
}
static uint8_t bytes[0x1000000], heap[16384];
static VcoGameModuleInfo modules[2];
static VcoGamePadSample sample = {.device = 1, .stamp = 100};
typedef struct {
    uint32_t address, width;
    uint8_t saved[4];
    int uid;
    bool live;
} Patch;
static Patch patches[VCO_ENGINE_CLAIMS + 16];
static unsigned next_patch;
static uint8_t *addressed(uint32_t address, uint32_t width)
{
    if (address >= BASE && (uint64_t)address + width <= (uint64_t)BASE + sizeof(bytes))
        return bytes + (address - BASE);
    if (address >= HEAP && (uint64_t)address + width <= (uint64_t)HEAP + sizeof(heap))
        return heap + (address - HEAP);
    return NULL;
}
int vcoAgentPermission(uint64_t session, uint32_t required)
{
    if (permission_busy) {
        --permission_busy;
        return VCO_AGENT_BUSY;
    }
    if (!live || session != binding.session)
        return VCO_AGENT_STALE;
    return required && !foreground ? VCO_AGENT_BUSY : VCO_AGENT_OK;
}
int vcoAgentRead(const VcoAgentMemoryRequest *request, uint32_t request_length)
{
    assert(request_length == sizeof(*request) && request->abi == VCO_AGENT_ABI &&
           !request->reserved);
    uint64_t session = request->session;
    uint32_t address = request->address, width = request->width;
    void *out = (void *)(uintptr_t)request->buffer;
    ++reads;
    if (read_busy) {
        --read_busy;
        return VCO_AGENT_BUSY;
    }
    int error = vcoAgentPermission(session, 1);
    if (error)
        return error;
    uint8_t *src = addressed(address, width);
    if (read_fail || !src)
        return VCO_AGENT_IO;
    memcpy(out, src, width);
    return 0;
}
int vcoAgentWrite(const VcoAgentMemoryRequest *request, uint32_t request_length)
{
    assert(request_length == sizeof(*request) && request->abi == VCO_AGENT_ABI &&
           !request->reserved);
    uint64_t session = request->session;
    uint32_t address = request->address, width = request->width;
    const void *input = (const void *)(uintptr_t)request->buffer;
    ++writes;
    int error = vcoAgentPermission(session, 1);
    if (error)
        return error;
    uint8_t *out = addressed(address, width);
    if (write_fail || !out)
        return VCO_AGENT_IO;
    memcpy(out, input, width);
    return 0;
}
int vcoAgentModules(uint64_t session, VcoGameModuleList *out, uint32_t length)
{
    assert(length == sizeof(*out));
    ++lists;
    if (list_busy) {
        --list_busy;
        return VCO_AGENT_BUSY;
    }
    int error = vcoAgentPermission(session, 0);
    if (error)
        return error;
    if (list_fail)
        return VCO_AGENT_IO;
    *out = (VcoGameModuleList){.count = 2, .ids = {11, change_list ? 33 : 22}};
    return 0;
}
int vcoAgentModuleInfo(const VcoAgentModuleRequest *request, uint32_t request_length)
{
    assert(request_length == sizeof(*request) && request->abi == VCO_AGENT_ABI &&
           !request->reserved);
    uint64_t session = request->session;
    int32_t uid = request->uid;
    VcoGameModuleInfo *out = (VcoGameModuleInfo *)(uintptr_t)request->output;
    uint32_t length = request->length;
    assert(length == sizeof(*out));
    ++infos;
    if (info_busy) {
        --info_busy;
        return VCO_AGENT_BUSY;
    }
    int error = vcoAgentPermission(session, 0);
    if (error)
        return error;
    if (info_fail)
        return VCO_AGENT_IO;
    if (uid != 11 && uid != 22)
        return VCO_AGENT_IO;
    *out = modules[uid == 22];
    return 0;
}
int vcoAgentPad(uint64_t session, VcoGamePadSample *out, uint32_t length)
{
    assert(length == sizeof(*out));
    int error = vcoAgentPermission(session, 1);
    if (!error)
        *out = sample;
    return error;
}
static int inject_at(uint32_t address, const void *input, size_t width)
{
    assert(width == 1 || width == 2 || width == 4);
    if (inject_fail || (fail_after && injections == fail_after))
        return -20;
    uint8_t *p = addressed(address, (uint32_t)width);
    assert(p && next_patch < VCO_ENGINE_CLAIMS + 16);
    for (unsigned i = 0; i < next_patch; ++i)
        if (patches[i].live &&
            (uint64_t)address < (uint64_t)patches[i].address + patches[i].width &&
            (uint64_t)patches[i].address < (uint64_t)address + width)
            return -21;
    Patch *r = &patches[next_patch];
    *r = (Patch){
        .address = address, .width = (uint32_t)width, .uid = (int)next_patch + 1000, .live = true};
    memcpy(r->saved, p, width);
    memcpy(p, input, width);
    ++next_patch;
    ++injections;
    if (change_on_inject)
        ++modules[1].version;
    if (background_on_inject)
        foreground = false;
    return r->uid;
}
int32_t taiInjectAbs(void *out, const void *input, size_t width)
{
    ++abs_injections;
    return inject_at((uint32_t)(uintptr_t)out, input, width);
}
int32_t taiInjectDataForUser(tai_offset_args_t *args)
{
    assert(args->size == sizeof(*args) && args->segidx >= 0 && args->segidx < 4);
    assert(args->modid == 10011 || args->modid == 10022); /* Process UID, not GUID. */
    VcoGameModuleInfo *m = &modules[args->modid == 10022];
    assert((uint64_t)args->offset + args->source_size <= m->segments[args->segidx].size);
    return inject_at(m->segments[args->segidx].base + args->offset, args->source,
                     args->source_size);
}
int taiInjectRelease(int32_t uid)
{
    assert(uid >= 1000 && (unsigned)(uid - 1000) < next_patch);
    Patch *p = &patches[uid - 1000];
    assert(p->live); /* Fail the test if an already consumed UID is retried. */
    p->live = false;
    ++releases;
    if (release_fail)
        return -30;
    memcpy(addressed(p->address, p->width), p->saved, p->width);
    return release_success;
}
static void setup(void)
{
    modules[0] = (VcoGameModuleInfo){.uid = 11,
                                     .process_uid = 10011,
                                     .version = 1,
                                     .state = 6,
                                     .name = "library",
                                     .segments = {{0x84000000, 512, 5}}};
    modules[1] = (VcoGameModuleInfo){.uid = 22,
                                     .process_uid = 10022,
                                     .version = 1,
                                     .state = 6,
                                     .name = "eboot",
                                     .segments = {{BASE, 0x800000, 5}, {DATA, 0x800000, 6}}};
    assert(!vco_game_native_memory_init(&native, &binding));
    memory = vco_game_native_memory_callbacks(&native);
    vco_game_native_memory_prepare(&native, binding.session);
    assert(native.mapped);
}
static void data_test(void)
{
    setup();
    uint8_t input[4] = {1, 2, 3, 4}, out[4] = {9, 9, 9, 9};
    assert(vco_game_native_memory_init(&native, &binding) == VCO_E_ARGUMENT);
    for (unsigned width = 1; width <= 4; width *= 2) {
        assert(!memory.write(&native, 1, DATA + 1, input, width));
        assert(!memory.read(&native, 1, DATA + 1, out, width));
        assert(!memcmp(out, input, width));
    }
    assert(memory.read(&native, 1, DATA, out, 3) != 0);
    read_fail = true;
    out[0] = 9;
    assert(memory.read(&native, 1, DATA, out, 4) != 0 && out[0] == 9);
    foreground = false;
    assert(memory.write(&native, 1, DATA, input, 4) == VCO_ACCESS_SESSION);
    assert(!memory.alive(&native, 1));
    assert(memory.read(&native, 2, DATA, out, 4) == VCO_ACCESS_SESSION);
}
static void modules_test(void)
{
    setup();
    uint32_t base = 9, size = 9;
    assert(!memory.module(&native, 1, 0, 0, &base, &size) && base == BASE && size == 0x800000);
    assert(!memory.module(&native, 1, 0, 1, &base, &size) && base == DATA);
    assert(!memory.module(&native, 1, 1, 0, &base, &size) && base == 0x84000000);
    base = size = 9;
    assert(memory.module(&native, 1, 2, 0, &base, &size) != 0 && base == 9 && size == 9);
    assert(memory.module(&native, 1, 0, 2, &base, &size) != 0);
    ++modules[1].version;
    assert(memory.module(&native, 1, 0, 0, &base, &size) != 0 && base == 9);
    assert(native.modules[1].version == 1); /* No implicit remapping on resolve. */
    vco_game_native_memory_prepare(&native, 1);
    assert(!memory.module(&native, 1, 0, 0, &base, &size));
    change_list = true;
    assert(memory.module(&native, 1, 0, 0, &base, &size) != 0);
}
static int32_t patch(uint32_t address)
{
    uint8_t value[4] = {1, 2, 3, 4};
    int32_t key = -1;
    assert(!memory.inject(&native, 1, address, value, 4, &key) && key > 0);
    return key;
}
static void patches_test(void)
{
    setup();
    bytes[1] = 77;
    int32_t key = patch(BASE + 1);
    assert(bytes[1] == 1 && injections == 1 && !abs_injections);
    unsigned sampled = lists;
    vco_game_native_memory_prepare(&native, 1);
    assert(lists == sampled); /* Active patch owners prevent new snapshots. */
    foreground = false;       /* Manual restore uses instance authority, not foreground. */
    assert(!memory.release(&native, 1, key));
    assert(bytes[1] == 77 && !native.owned && releases == 1);
    assert(memory.release(&native, 1, key) != 0 && releases == 1);
    assert(!vco_game_native_memory_finalize(&native));
}
static void absolute_test(void)
{
    setup();
    heap[1] = 88;
    int32_t key = patch(HEAP + 1);
    assert(abs_injections == 1 && heap[1] == 1);
    assert(!memory.release(&native, 1, key) && heap[1] == 88);
}
static void private_test(void)
{
    setup();
    uint8_t value[4] = {1, 2, 3, 4};
    int32_t key = 999;
    change_on_inject = true;
    assert(memory.inject(&native, 1, BASE, value, 4, &key) != 0 && key == 999);
    assert(native.owned == 1 && native.blocked && !releases);
    assert(vco_game_native_memory_finalize(&native) == VCO_E_RESTORE && !releases);
    --modules[1].version;
    change_on_inject = false;
    assert(!vco_game_native_memory_finalize(&native) && releases == 1 && !native.owned);
    vco_game_native_memory_prepare(&native, 1);
    background_on_inject = true;
    assert(memory.inject(&native, 1, BASE, value, 4, &key) == VCO_ACCESS_SESSION && key == 999);
    assert(!native.owned && releases == 2 && !bytes[0]);
}
static void release_failure(void)
{
    setup();
    int32_t key = patch(BASE);
    release_fail = true;
    assert(memory.release(&native, 1, key) != 0 && releases == 1);
    release_fail = false;
    assert(vco_game_native_memory_finalize(&native) == VCO_E_RESTORE && releases == 1);
    assert(!memory.alive(&native, 1));
}
static void release_positive(void)
{
    setup();
    bytes[1] = 77;
    int32_t key = patch(BASE + 1);
    release_success = 4;
    assert(!memory.release(&native, 1, key));
    assert(bytes[1] == 77 && !native.owned && !native.blocked);
    assert(diagnostics[diagnostic_count - 1].event == VCO_GAME_TRACE_RELEASE &&
           diagnostics[diagnostic_count - 1].result == 4);
}
static void diagnostic_budget(void)
{
    setup();
    uint8_t value[4] = {1, 2, 3, 4};
    int32_t key = 999;
    diagnostic_result = VCO_AGENT_BUSY;
    change_on_inject = true;
    assert(memory.inject(&native, 1, BASE, value, 4, &key) != 0 && key == 999);
    for (unsigned i = 0; i < 40; ++i)
        assert(vco_game_native_memory_finalize(&native) == VCO_E_RESTORE);
    assert(diagnostic_count == 112 && native.diagnostics == 14 && native.owned == 1 && !releases);
    --modules[1].version;
    assert(!vco_game_native_memory_finalize(&native) && releases == 1);
}
static void revoked_test(void)
{
    setup();
    int32_t key = patch(BASE);
    live = false;
    assert(memory.release(&native, 1, key) != 0 && !releases);
    assert(vco_game_native_memory_finalize(&native) == VCO_E_RESTORE && !releases);
    assert(bytes[0] == 1); /* No old-address write when session has ended. */
}
static void capacity_test(void)
{
    setup();
    uint8_t value = 1;
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i) {
        int32_t key;
        assert(!memory.inject(&native, 1, HEAP + i, &value, 1, &key));
    }
    int32_t key = 777;
    assert(memory.inject(&native, 1, HEAP + VCO_ENGINE_CLAIMS, &value, 1, &key) != 0);
    assert(key == 777 && native.owned == VCO_ENGINE_CLAIMS && injections == VCO_ENGINE_CLAIMS);
    assert(!vco_game_native_memory_finalize(&native) && releases == VCO_ENGINE_CLAIMS);
    vco_game_native_memory_prepare(&native, 1);
    assert(!memory.inject(&native, 1, HEAP, &value, 1, &key) && key > (int32_t)VCO_ENGINE_CLAIMS);
    assert(!memory.release(&native, 1, key));
}
static void busy_init(void)
{
    permission_busy = 2;
    setup();
    assert(native.initialized && native.mapped && delays == 2);
}
static void busy_restore(void)
{
    setup();
    int32_t key = patch(BASE);
    permission_busy = 2;
    info_busy = 2;
    assert(!memory.release(&native, 1, key));
    assert(!native.owned && !native.blocked && releases == 1 && delays == 4);
    assert(memory.release(&native, 1, key) != 0 && releases == 1);
}
static void busy_snapshot(void)
{
    setup();
    permission_busy = list_busy = info_busy = 2;
    vco_game_native_memory_prepare(&native, 1);
    assert(native.mapped && !native.blocked && delays == 6);
    read_busy = 2;
    uint8_t out[4];
    assert(!memory.read(&native, 1, DATA, out, sizeof(out)) && delays == 8);
}
static void busy_exhaustion(void)
{
    setup();
    int32_t key = patch(BASE);
    permission_busy = 100;
    assert(memory.release(&native, 1, key) != 0);
    assert(delays == 7 && releases == 0 && native.owned == 1 && native.blocked);
    bool found = false;
    for (unsigned i = 0; i < diagnostic_count; ++i)
        if (diagnostics[i].event == VCO_GAME_TRACE_QUERY && diagnostics[i].result == VCO_AGENT_BUSY)
            found = true;
    assert(found);
}
static void busy_revoke(void)
{
    setup();
    int32_t key = patch(BASE);
    permission_busy = 1;
    revoke_on_delay = true;
    assert(memory.release(&native, 1, key) != 0);
    assert(delays == 1 && !live && !releases && native.blocked && bytes[0] == 1);
}
static void busy_write(void)
{
    setup();
    permission_busy = 1;
    uint8_t in[4] = {1, 2, 3, 4};
    assert(memory.write(&native, 1, DATA, in, sizeof(in)) == VCO_ACCESS_SESSION);
    assert(writes == 1 && !delays && !bytes[0x800000]);
}
static void trace_recovery(void)
{
    setup();
    diagnostic_busy = 2;
    int32_t key = patch(BASE);
    assert(diagnostic_count == 3 && delays == 2);
    assert(diagnostics[2].event == VCO_GAME_TRACE_INJECT);
    assert(!memory.release(&native, 1, key));
}
static void pad_test(void)
{
    setup();
    uint32_t device = 9, buttons = 9;
    assert(memory.pad(&native, 1, &device, &buttons) == VCO_PAD_SUPPRESSED && buttons == 9);
    sample.buttons = 0x8000;
    ++sample.stamp;
    assert(!memory.pad(&native, 1, &device, &buttons) && buttons == 0x8000 && device == 1);
    sample.capturing = 1;
    assert(memory.pad(&native, 1, &device, &buttons) == VCO_PAD_SUPPRESSED);
    sample.capturing = 0;
    ++sample.capture_epoch;
    assert(memory.pad(&native, 1, &device, &buttons) == VCO_PAD_SUPPRESSED);
    sample.buttons = 0;
    assert(memory.pad(&native, 1, &device, &buttons) == VCO_PAD_SUPPRESSED);
    sample.buttons = 0x8000;
    ++sample.stamp;
    assert(!memory.pad(&native, 1, &device, &buttons));
    sample.buttons = 0x10000;
    assert(memory.pad(&native, 1, &device, &buttons) == VCO_PAD_SUPPRESSED);
    sample.buttons = 0x400;
    assert(memory.pad(&native, 1, &device, &buttons) != 0);
}
static void snapshot_failure(void)
{
    setup();
    list_fail = true;
    vco_game_native_memory_prepare(&native, 1);
    assert(!native.mapped);
    uint32_t base = 0, size = 0;
    assert(memory.module(&native, 1, 0, 0, &base, &size) != 0);
    uint8_t value[4] = {1, 2, 3, 4};
    assert(!memory.write(&native, 1, DATA, value, 4)); /* Data doesn't need B metadata. */
    list_fail = false;
    info_fail = true;
    vco_game_native_memory_prepare(&native, 1);
    assert(!native.mapped);
    info_fail = false;
    vco_game_native_memory_prepare(&native, 1);
    assert(native.mapped);
}
static VcoGameExecutor executor;
static VcoCheatEntry entries[VCO_ENGINE_ENTRIES];
static VcoCode codes[VCO_UPLOAD_CODES];
static char names[65536];
static uint8_t upload[VCO_UPLOAD_MAX];
static VcoCheatFile file = {entries, VCO_ENGINE_ENTRIES, 0, codes, VCO_UPLOAD_CODES, 0,
                            names,   sizeof(names),      0};
static uint64_t ticket, clock_us = 50000;
static void *allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}
static void deallocate(void *context, void *p)
{
    (void)context;
    free(p);
}
static void executor_init(void)
{
    setup();
    assert(!vco_game_executor_init(&executor, &binding,
                                   (VcoAgentAllocator){NULL, allocate, deallocate}, memory,
                                   vco_game_native_memory_finalize, &native));
    assert(!vco_game_executor_set_prepare(&executor, vco_game_native_memory_prepare, &native));
}
static void load_file(uint64_t request)
{
    size_t length;
    assert(!vco_upload_encode(upload, sizeof(upload), request, 1, (uint32_t)request, binding.pid,
                              binding.title_id, &file, &length));
    VcoAgentMessage message = {.session = 1,
                               .ticket = ++ticket,
                               .abi = VCO_AGENT_ABI,
                               .size = sizeof(message),
                               .operation = VCO_AGENT_LOAD,
                               .length = (uint32_t)length,
                               .runnable = 1};
    VcoAgentResult result;
    assert(!vco_game_executor_handle(&executor, &message, upload, length, clock_us, &result));
    assert(!result.error);
}
static void toggle(unsigned entry, bool enabled)
{
    VcoControlRequest request = {.request = ticket + 1,
                                 .session = 1,
                                 .revision = executor.engine.revision,
                                 .abi = VCO_CONTROL_ABI,
                                 .size = sizeof(request),
                                 .operation = VCO_CONTROL_SET,
                                 .entry = entry,
                                 .enabled = enabled,
                                 .count = 1};
    VcoAgentMessage message = {.session = 1,
                               .ticket = ++ticket,
                               .abi = VCO_AGENT_ABI,
                               .size = sizeof(message),
                               .operation = VCO_AGENT_CONTROL,
                               .length = sizeof(request),
                               .runnable = foreground};
    VcoAgentResult result;
    clock_us += VCO_ENGINE_PERIOD_US;
    assert(!vco_game_executor_handle(&executor, &message, &request, sizeof(request), clock_us,
                                     &result));
    assert(!result.error &&
           executor.engine.rows[entry].state == (enabled ? VCO_ENTRY_ON : VCO_ENTRY_OFF));
}
static void executor_test(void)
{
    executor_init();
    const char text[] = "_V1 Code\n$B200 00000000 00000000\n$A200 00000004 11223344\n";
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    load_file(1);
    assert(!native.mapped && executor.engine.rows[0].state == VCO_ENTRY_OFF);
    toggle(0, true);
    assert(injections == 1 && bytes[4] == 0x44);
    foreground = false;
    toggle(0, false);
    assert(releases == 1 && !bytes[4]);
    foreground = true;
    load_file(2);
    assert(!native.mapped);
    toggle(0, true);
    assert(injections == 2);
    assert(!vco_game_executor_stop(&executor) && releases == 2);
}
static void partial_failure(bool restore_failure)
{
    executor_init();
    const char text[] = "_V0 Energy prefix\n$B200 00000000 00000000\n"
                        "$A200 0013970A 4604B570\n$A200 0013970E F7FFB120\n"
                        "$A200 00139712 4620FFB8\n$A200 00139716 FCE5F145\n"
                        "$A200 0013971A F000BD70\n$A200 0013971E B1A0F817\n";
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    load_file(1);
    fail_after = 5;
    release_fail = restore_failure;
    VcoControlRequest request = {.request = ticket + 1,
                                 .session = 1,
                                 .revision = executor.engine.revision,
                                 .abi = VCO_CONTROL_ABI,
                                 .size = sizeof(request),
                                 .operation = VCO_CONTROL_SET,
                                 .enabled = 1,
                                 .count = 1};
    VcoAgentMessage message = {.session = 1,
                               .ticket = ++ticket,
                               .abi = VCO_AGENT_ABI,
                               .size = sizeof(message),
                               .operation = VCO_AGENT_CONTROL,
                               .length = sizeof(request),
                               .runnable = 1};
    VcoAgentResult result;
    assert(!vco_game_executor_handle(&executor, &message, &request, sizeof(request),
                                     clock_us + VCO_ENGINE_PERIOD_US, &result));
    assert(injections == 5 && releases == 5 && executor.engine.rows[0].state == VCO_ENTRY_ERROR);
    assert(executor.engine.rows[0].diagnostic.code ==
           (restore_failure ? VCO_E_RESTORE : VCO_E_PATCH));
    assert(executor.engine.rows[0].diagnostic.line == 8);
    bool initiating_error = false, cleanup_error = false;
    for (unsigned i = 0; i < diagnostic_count; ++i) {
        if (diagnostics[i].event == VCO_GAME_TRACE_INJECT && diagnostics[i].result == -20) {
            initiating_error = true;
            assert(diagnostics[i].subject == BASE + 0x13971e);
        }
        if (diagnostics[i].event == VCO_GAME_TRACE_RELEASE && diagnostics[i].result == -30)
            cleanup_error = true;
    }
    assert(initiating_error && cleanup_error == restore_failure);
    assert(native.owned == (restore_failure ? 5u : 0u));
    if (!restore_failure)
        for (unsigned i = 0; i < next_patch; ++i)
            assert(!memcmp(addressed(patches[i].address, patches[i].width), patches[i].saved,
                           patches[i].width));
}
static void neutral_test(void)
{
    executor_init();
    const char text[] = "_V0 neutral\n$C201 00000000 00000000\n$0200 81800004 11223344\n";
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    load_file(1);
    toggle(0, true);
    assert(!bytes[0x800004]); /* First neutral sample only acknowledges release. */
    VcoAgentMessage idle = {
        .session = 1, .abi = VCO_AGENT_ABI, .size = sizeof(idle), .runnable = 1};
    VcoAgentResult result;
    ++sample.stamp;
    clock_us += VCO_ENGINE_PERIOD_US;
    assert(!vco_game_executor_handle(&executor, &idle, NULL, 0, clock_us, &result));
    assert(bytes[0x800004] == 0x44); /* C mask zero matches fresh neutral input. */
    sample.capturing = 1;
    bytes[0x800004] = 0;
    clock_us += VCO_ENGINE_PERIOD_US;
    assert(!vco_game_executor_handle(&executor, &idle, NULL, 0, clock_us, &result));
    assert(!bytes[0x800004]); /* Overlay capture cannot masquerade as zero input. */
    sample.capturing = 0;
    ++sample.capture_epoch;
    ++sample.stamp;
    clock_us += VCO_ENGINE_PERIOD_US;
    assert(!vco_game_executor_handle(&executor, &idle, NULL, 0, clock_us, &result));
    assert(!bytes[0x800004]);
    ++sample.stamp;
    clock_us += VCO_ENGINE_PERIOD_US;
    assert(!vco_game_executor_handle(&executor, &idle, NULL, 0, clock_us, &result));
    assert(bytes[0x800004] == 0x44);
    assert(!vco_game_executor_stop(&executor));
}
static void fixture_test(const char *path)
{
    executor_init();
    FILE *input = fopen(path, "rb");
    assert(input && !fseek(input, 0, SEEK_END));
    long size = ftell(input);
    assert(size > 0 && !fseek(input, 0, SEEK_SET));
    char *text = malloc((size_t)size);
    assert(text && fread(text, 1, (size_t)size, input) == (size_t)size);
    fclose(input);
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, (size_t)size, &file, &diagnostic));
    free(text);
    assert(file.entry_count == 31 && file.code_count == 982);
    load_file(1);
    toggle(30, true);
    assert(injections == 198 && native.owned == 198 && !abs_injections);
    for (size_t i = entries[30].first + 1; i < entries[30].first + entries[30].count; ++i) {
        uint8_t *p = addressed(BASE + codes[i].a, 4);
        assert(p && p[0] == (uint8_t)codes[i].b && p[1] == (uint8_t)(codes[i].b >> 8));
    }
    toggle(30, false);
    assert(releases == 198 && !native.owned);
    for (unsigned i = 0; i < next_patch; ++i) {
        uint8_t *p = addressed(patches[i].address, patches[i].width);
        assert(!memcmp(p, patches[i].saved, patches[i].width));
    }
    assert(!vco_game_executor_stop(&executor));
}
static void energy_fixture(const char *path)
{
    executor_init();
    FILE *input = fopen(path, "rb");
    assert(input && !fseek(input, 0, SEEK_END));
    long size = ftell(input);
    assert(size > 0 && !fseek(input, 0, SEEK_SET));
    char *text = malloc((size_t)size);
    assert(text && fread(text, 1, (size_t)size, input) == (size_t)size);
    fclose(input);
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, (size_t)size, &file, &diagnostic));
    free(text);
    assert(file.entry_count == 1 && file.code_count == 50);
    load_file(1);
    permission_busy = list_busy = info_busy = read_busy = 1;
    toggle(0, true);
    assert(injections == 49 && native.owned == 49 && !abs_injections);
    unsigned small = 0;
    for (unsigned i = 0; i < next_patch; ++i)
        small += patches[i].width == 2;
    assert(small == 3 && delays == 4);
    for (size_t i = 1; i < file.code_count; ++i) {
        uint8_t *p = addressed(BASE + codes[i].a, 4);
        assert(p && p[0] == (uint8_t)codes[i].b && p[1] == (uint8_t)(codes[i].b >> 8));
    }
    permission_busy = info_busy = 1;
    toggle(0, false);
    assert(releases == 49 && !native.owned && !native.blocked && delays == 6);
    for (unsigned i = 0; i < next_patch; ++i)
        assert(!memcmp(addressed(patches[i].address, patches[i].width), patches[i].saved,
                       patches[i].width));
    assert(!vco_game_executor_stop(&executor));
}
static void trajectory_fixture(const char *path, const char *mode)
{
    executor_init();
    FILE *input = fopen(path, "rb");
    assert(input && !fseek(input, 0, SEEK_END));
    long size = ftell(input);
    assert(size > 0 && !fseek(input, 0, SEEK_SET));
    char *text = malloc((size_t)size);
    assert(text && fread(text, 1, (size_t)size, input) == (size_t)size);
    fclose(input);
    VcoDiagnostic diagnostic;
    assert(!vco_cheat_parse(text, (size_t)size, &file, &diagnostic));
    free(text);
    assert(file.entry_count == 1 && file.code_count == 200 && codes[40].line == 2087);
    load_file(1);
    toggle(0, true);
    assert(injections == 199 && native.owned == 199);
    VcoAgentMessage idle = {
        .session = 1, .abi = VCO_AGENT_ABI, .size = sizeof(idle), .runnable = 1};
    VcoAgentResult result;
    for (unsigned i = 0; i < 60; ++i) {
        clock_us += VCO_ENGINE_PERIOD_US;
        assert(!vco_game_executor_handle(&executor, &idle, NULL, 0, clock_us, &result));
    }
    assert(injections == 199 && !releases);
    foreground = false;
    bool normal = !strcmp(mode, "trajectory_pause");
    bool changed = !strcmp(mode, "trajectory_changed");
    if (!normal) {
        /* Routine foreground denials must not hide the first actual restore
         * failure. Exercise the real adapter, runtime and engine together. */
        for (unsigned i = 0; i < 32; ++i)
            assert(!memory.alive(&native, binding.session));
        if (!strcmp(mode, "trajectory_guard"))
            info_busy = 8;
        else if (changed)
            ++modules[1].version;
        else
            release_fail = true;
    } else
        info_busy = 2;
    idle.runnable = 0;
    clock_us += VCO_ENGINE_PERIOD_US;
    assert(!vco_game_executor_handle(&executor, &idle, NULL, 0, clock_us, &result));
    if (normal) {
        assert(releases == 199 && !native.owned && !native.blocked);
        assert(executor.engine.rows[0].state == VCO_ENTRY_PAUSED);
        foreground = true;
        idle.runnable = 1;
        clock_us += VCO_ENGINE_PERIOD_US;
        assert(!vco_game_executor_handle(&executor, &idle, NULL, 0, clock_us, &result));
        assert(injections == 398 && native.owned == 199 &&
               executor.engine.rows[0].state == VCO_ENTRY_ON);
        toggle(0, false);
        assert(releases == 398 && !native.owned && !native.blocked);
        assert(!vco_game_executor_stop(&executor));
    } else {
        assert(executor.engine.rows[0].state == VCO_ENTRY_ERROR &&
               executor.engine.rows[0].diagnostic.code == VCO_E_RESTORE && native.blocked);
        unsigned wanted = (!strcmp(mode, "trajectory_guard") || changed)
                              ? VCO_GAME_TRACE_RELEASE_GUARD
                              : VCO_GAME_TRACE_RELEASE;
        unsigned failures = 0;
        bool original_query = false;
        bool original_difference = false;
        for (unsigned i = 0; i < diagnostic_count; ++i) {
            failures += diagnostics[i].event == wanted && diagnostics[i].result < 0;
            if (diagnostics[i].event == VCO_GAME_TRACE_QUERY &&
                diagnostics[i].result == VCO_AGENT_BUSY && diagnostics[i].detail == (3u << 16))
                original_query = true;
            if (diagnostics[i].event == VCO_GAME_TRACE_GUARD && diagnostics[i].subject == 10022 &&
                diagnostics[i].result == (1 << 8 | 2) &&
                diagnostics[i].detail == offsetof(VcoGameModuleInfo, version))
                original_difference = true;
        }
        assert(failures == 1 && diagnostic_count <= 16);
        if (changed) {
            assert(original_difference && !releases && native.owned == 199);
            --modules[1].version;
            assert(!vco_game_native_memory_finalize(&native));
            assert(releases == 199 && !native.owned);
        } else if (wanted == VCO_GAME_TRACE_RELEASE_GUARD) {
            assert(original_query && releases == 198 && native.owned == 1);
            info_busy = 0;
        } else {
            assert(releases == 199 && native.owned == 199);
            release_fail = false;
            unsigned before = releases;
            assert(vco_game_native_memory_finalize(&native) == VCO_E_RESTORE);
            assert(releases == before); /* Consumed failing UIDs are never retried. */
        }
    }
    for (unsigned i = 0; normal && i < next_patch; ++i)
        assert(!memcmp(addressed(patches[i].address, patches[i].width), patches[i].saved,
                       patches[i].width));
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    if (!strcmp(argv[1], "busy_init"))
        busy_init();
    else if (!strcmp(argv[1], "busy_restore"))
        busy_restore();
    else if (!strcmp(argv[1], "busy_snapshot"))
        busy_snapshot();
    else if (!strcmp(argv[1], "busy_exhaustion"))
        busy_exhaustion();
    else if (!strcmp(argv[1], "busy_revoke"))
        busy_revoke();
    else if (!strcmp(argv[1], "busy_write"))
        busy_write();
    else if (!strcmp(argv[1], "trace_recovery"))
        trace_recovery();
    else if (!strcmp(argv[1], "energy"))
        energy_fixture(argv[2]);
    else if (!strncmp(argv[1], "trajectory_", 11))
        trajectory_fixture(argv[2], argv[1]);
    else if (!strcmp(argv[1], "data"))
        data_test();
    else if (!strcmp(argv[1], "modules"))
        modules_test();
    else if (!strcmp(argv[1], "patches"))
        patches_test();
    else if (!strcmp(argv[1], "absolute"))
        absolute_test();
    else if (!strcmp(argv[1], "private"))
        private_test();
    else if (!strcmp(argv[1], "release_failure"))
        release_failure();
    else if (!strcmp(argv[1], "release_positive"))
        release_positive();
    else if (!strcmp(argv[1], "diagnostic_budget"))
        diagnostic_budget();
    else if (!strcmp(argv[1], "partial_patch_failure"))
        partial_failure(false);
    else if (!strcmp(argv[1], "partial_restore_failure"))
        partial_failure(true);
    else if (!strcmp(argv[1], "revoked"))
        revoked_test();
    else if (!strcmp(argv[1], "capacity"))
        capacity_test();
    else if (!strcmp(argv[1], "pad"))
        pad_test();
    else if (!strcmp(argv[1], "neutral"))
        neutral_test();
    else if (!strcmp(argv[1], "snapshot_failure"))
        snapshot_failure();
    else if (!strcmp(argv[1], "executor"))
        executor_test();
    else if (!strcmp(argv[1], "fixture"))
        fixture_test(argv[2]);
    else
        assert(!"unknown case");
    if (executor.arena)
        free(executor.arena);
    printf("game native memory %s passed\n", argv[1]);
    return 0;
}
