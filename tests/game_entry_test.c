#define _POSIX_C_SOURCE 200809L
#include "agent/vita_allocator.h"
#include "agent/vita_memory.h"
#include "agent/vita_worker.h"
#include "agent/native_service.h"
#include "agent/native_modules.h"
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <taihen.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
int module_start(SceSize, void *);
int module_stop(SceSize, void *);
#define BASE UINT32_C(0x81000000)
static VcoAgentBinding binding = {.session = 1,
                                  .abi = VCO_AGENT_ABI,
                                  .size = sizeof(binding),
                                  .pid = 77,
                                  .title_id = "PCSB00031"};
static struct {
    void *base;
    size_t bytes;
    bool live;
} blocks[32];
static unsigned allocations, frees, creates, starts, deletes, waits, reports, polls, publishes;
static unsigned stopped_receipts;
static int stopped_delivery_error;
static int alloc_error, base_error, free_error, create_error, start_error, delete_error, wait_error;
static int attach_error;
static unsigned attach_calls, attach_busy, startup_delays;
static bool restore_error, revoked_mode, fault_mode, leave_on;
static bool patch_live, thread_ran;
static unsigned restores;
static uint32_t game[8], saved;
static VcoError reported;
static VcoGameWorker *worker;
static SceKernelThreadEntry thread_entry;
static VcoAgentResult last;
static bool boundary_mode, boundary_launch;
static pthread_t boundary_thread;
static pthread_mutex_t boundary_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t boundary_condition = PTHREAD_COND_INITIALIZER;
static VcoAgentBroker boundary_broker;
static uint8_t boundary_upload[VCO_UPLOAD_MAX];
static int (*boundary_callbacks[8])(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static unsigned boundary_hooks, boundary_originals, boundary_unloads;
static bool boundary_module_stopped;
static void *boundary_entry(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&boundary_mutex);
    while (!boundary_launch)
        pthread_cond_wait(&boundary_condition, &boundary_mutex);
    pthread_mutex_unlock(&boundary_mutex);
    return (void *)(intptr_t)thread_entry(sizeof(worker), &worker);
}
int sceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *options)
{
    assert(!strcmp(name, "vco_game_data") && type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !options &&
           bytes && !(bytes & 4095u));
    if (alloc_error)
        return alloc_error;
    assert(allocations < 32);
    unsigned i = allocations++;
    assert(!posix_memalign(&blocks[i].base, 4096, bytes));
    blocks[i].bytes = bytes;
    blocks[i].live = true;
    return (int)i + 100;
}
int sceKernelGetMemBlockBase(int uid, void **out)
{
    assert(uid >= 100 && (unsigned)(uid - 100) < allocations && blocks[uid - 100].live);
    if (base_error)
        return base_error;
    *out = blocks[uid - 100].base;
    return 0;
}
int sceKernelFreeMemBlock(int uid)
{
    assert(uid >= 100 && (unsigned)(uid - 100) < allocations && blocks[uid - 100].live);
    ++frees;
    if (free_error)
        return free_error;
    free(blocks[uid - 100].base);
    blocks[uid - 100].base = NULL;
    blocks[uid - 100].live = false;
    return 0;
}
int sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority,
                          unsigned stack, unsigned attributes, int affinity, const void *options)
{
    assert(!strcmp(name, "vco_game_execution") && stack == 0x6000 && !attributes && !options);
    (void)priority;
    (void)affinity;
    ++creates;
    thread_entry = entry;
    return create_error ? create_error : 12;
}
int sceKernelStartThread(int uid, SceSize size, void *arguments)
{
    assert(uid == 12 && size == sizeof(worker));
    memcpy(&worker, arguments, size);
    ++starts;
    if (boundary_mode)
        assert(!pthread_create(&boundary_thread, NULL, boundary_entry, NULL));
    return start_error;
}
int sceKernelWaitThreadEnd(int uid, int *status, SceUInt *timeout)
{
    assert(uid == 12 && timeout && *timeout == 1000 && (thread_ran || boundary_mode));
    ++waits;
    if (boundary_mode && !wait_error) {
        void *thread_result = NULL;
        assert(__atomic_load_n(&worker->completed, __ATOMIC_ACQUIRE));
        assert(!pthread_join(boundary_thread, &thread_result));
        *status = (int)(intptr_t)thread_result;
    } else if (!wait_error)
        *status = worker->run_error;
    return wait_error;
}
int sceKernelDeleteThread(int uid)
{
    assert(uid == 12);
    ++deletes;
    return delete_error;
}
int sceKernelDelayThread(SceUInt period)
{
    assert(period == VCO_GAME_WORKER_PERIOD_US || period == 1000);
    if (!creates)
        ++startup_delays;
    if (boundary_mode) {
        struct timespec delay = {0, (long)period * 1000};
        assert(!nanosleep(&delay, NULL));
        return 0;
    }
    assert(polls < 20);
    return 0;
}
SceInt64 sceKernelGetSystemTimeWide(void)
{
    return (SceInt64)polls * 50000;
}
int vcoAgentAttach(const VcoAgentBinding *input, uint32_t size)
{
    assert(size == sizeof(binding) && !memcmp(input, &binding, size));
    ++attach_calls;
    if (attach_busy) {
        --attach_busy;
        return VCO_AGENT_BUSY;
    }
    if (boundary_mode)
        return vco_broker_attach(&boundary_broker, 77, input);
    return attach_error;
}
int vcoAgentPermission(uint64_t session, uint32_t foreground)
{
    assert(session == binding.session && foreground <= 1);
    if (boundary_mode) {
        pthread_mutex_lock(&boundary_mutex);
        int error = !boundary_broker.attached                                ? VCO_AGENT_CLOSED
                    : foreground && vco_broker_module_busy(&boundary_broker) ? VCO_AGENT_BUSY
                                                                             : VCO_AGENT_OK;
        pthread_mutex_unlock(&boundary_mutex);
        return error;
    }
    return revoked_mode && polls >= 4 ? VCO_AGENT_STALE : VCO_AGENT_OK;
}
int vcoAgentFault(uint64_t session, uint32_t error)
{
    assert(session == binding.session && error && error <= VCO_E_METADATA);
    ++reports;
    reported = (VcoError)error;
    if (boundary_mode) {
        pthread_mutex_lock(&boundary_mutex);
        boundary_broker.attached = false;
        boundary_broker.status.state = VCO_SERVICE_ERROR;
        boundary_broker.status.error = error;
        pthread_cond_broadcast(&boundary_condition);
        pthread_mutex_unlock(&boundary_mutex);
    }
    return VCO_AGENT_OK;
}
int vcoAgentTrace(const VcoAgentTraceRequest *request, uint32_t length)
{
    assert(length == sizeof(*request) && request->size == length && request->abi == VCO_AGENT_ABI &&
           request->session == binding.session);
    return VCO_AGENT_OK;
}
int vcoAgentRead(const VcoAgentMemoryRequest *request, uint32_t request_length)
{
    assert(request_length == sizeof(*request) && request->abi == VCO_AGENT_ABI &&
           !request->reserved);
    uint64_t session = request->session;
    uint32_t address = request->address, width = request->width;
    void *out = (void *)(uintptr_t)request->buffer;
    assert(!vcoAgentPermission(session, 1) && address >= BASE &&
           (uint64_t)address + width <= BASE + sizeof(game));
    memcpy(out, (unsigned char *)game + address - BASE, width);
    return 0;
}
int vcoAgentWrite(const VcoAgentMemoryRequest *request, uint32_t request_length)
{
    assert(request_length == sizeof(*request) && request->abi == VCO_AGENT_ABI &&
           !request->reserved);
    uint64_t session = request->session;
    uint32_t address = request->address, width = request->width;
    const void *input = (const void *)(uintptr_t)request->buffer;
    assert(!vcoAgentPermission(session, 1) && address >= BASE &&
           (uint64_t)address + width <= BASE + sizeof(game));
    memcpy((unsigned char *)game + address - BASE, input, width);
    return 0;
}
int vcoAgentModules(uint64_t session, VcoGameModuleList *out, uint32_t size)
{
    assert(!vcoAgentPermission(session, 0) && size == sizeof(*out));
    *out = (VcoGameModuleList){.count = 1, .ids = {11}};
    return 0;
}
int vcoAgentModuleInfo(const VcoAgentModuleRequest *request, uint32_t request_length)
{
    assert(request_length == sizeof(*request) && request->abi == VCO_AGENT_ABI &&
           !request->reserved);
    uint64_t session = request->session;
    int32_t uid = request->uid;
    VcoGameModuleInfo *out = (VcoGameModuleInfo *)(uintptr_t)request->output;
    uint32_t size = request->length;
    assert(!vcoAgentPermission(session, 0) && uid == 11 && size == sizeof(*out));
    *out = (VcoGameModuleInfo){
        .uid = 11, .process_uid = 111, .name = "Game", .segments = {{BASE, sizeof(game), 5}}};
    return 0;
}
int vcoAgentPad(uint64_t session, VcoGamePadSample *out, uint32_t size)
{
    assert(!vcoAgentPermission(session, 1) && size == sizeof(*out));
    *out = (VcoGamePadSample){.device = 1, .stamp = polls};
    return 0;
}
int32_t taiInjectAbs(void *address, const void *input, size_t width)
{
    assert((uintptr_t)address == BASE + 8 && width == 4 && !patch_live);
    saved = game[2];
    memcpy(&game[2], input, width);
    patch_live = true;
    return 700;
}
int32_t taiInjectDataForUser(tai_offset_args_t *args)
{
    assert(args->size == sizeof(*args) && args->modid == 111 && args->segidx == 0);
    return taiInjectAbs((void *)(uintptr_t)(BASE + args->offset), args->source, args->source_size);
}
int taiInjectRelease(int32_t uid)
{
    assert(uid == 700 && patch_live);
    patch_live = false; /* The UID is consumed, including on copy failure. */
    ++restores;
    if (restore_error)
        return -22;
    game[2] = saved;
    return 0;
}
static size_t packet(void *bytes)
{
    VcoCheatEntry entries[2];
    VcoCode codes[3];
    char names[32];
    VcoCheatFile file = {entries, 2, 0, codes, 3, 0, names, sizeof(names), 0};
    VcoDiagnostic diagnostic;
    const char text[] = "_V1 Data\n$0200 81000000 00000040\n"
                        "_V1 Patch\n$B200 00000000 00000000\n$A200 00000008 00000099\n";
    assert(!vco_cheat_parse(text, strlen(text), &file, &diagnostic));
    size_t size;
    assert(!vco_upload_encode(bytes, VCO_UPLOAD_MAX, 1, 1, 1, 77, binding.title_id, &file, &size));
    return size;
}
int vcoAgentPoll(const VcoAgentPollRequest *request, uint32_t request_length)
{
    assert(request_length == sizeof(*request) && request->abi == VCO_AGENT_ABI &&
           !request->reserved);
    uint64_t session = request->session;
    VcoAgentMessage *out = (VcoAgentMessage *)(uintptr_t)request->message;
    uint32_t size = request->message_length, capacity = request->capacity;
    void *bytes = (void *)(uintptr_t)request->payload;
    assert(session == 1 && size == sizeof(*out) && capacity == VCO_UPLOAD_MAX);
    ++polls;
    if (boundary_mode) {
        pthread_mutex_lock(&boundary_mutex);
        int e = vco_broker_poll(&boundary_broker, 77, session, out, bytes, capacity);
        pthread_mutex_unlock(&boundary_mutex);
        return e;
    }
    if (polls == 4 && (revoked_mode || fault_mode))
        return revoked_mode ? VCO_AGENT_STALE : VCO_AGENT_IO;
    *out = (VcoAgentMessage){
        .session = 1, .abi = VCO_AGENT_ABI, .size = sizeof(*out), .runnable = 1, .ticket = polls};
    if (polls == 1) {
        out->operation = VCO_AGENT_LOAD;
        out->length = (uint32_t)packet(bytes);
    } else {
        out->operation = VCO_AGENT_CONTROL;
        out->length = sizeof(VcoControlRequest);
        VcoControlRequest request = {.request = polls,
                                     .session = 1,
                                     .revision = 1,
                                     .abi = VCO_CONTROL_ABI,
                                     .size = sizeof(request),
                                     .operation = VCO_CONTROL_SET,
                                     .entry = polls == 2 ? 0 : 1,
                                     .enabled = polls < 4,
                                     .count = 1};
        memcpy(bytes, &request, sizeof(request));
    }
    return 0;
}
int vcoAgentPublish(const VcoAgentResult *input, uint32_t size)
{
    assert(size == sizeof(*input));
    if (boundary_mode) {
        pthread_mutex_lock(&boundary_mutex);
        int e = vco_broker_publish(&boundary_broker, 77, input);
        if (!e && input->operation != VCO_AGENT_CONTROL && input->operation != VCO_AGENT_STOPPED)
            assert(!vco_broker_ack(&boundary_broker, 99, input->ticket));
        if (!e && input->operation == VCO_AGENT_STOPPED) {
            assert(worker->executor->stopped && !patch_live && !worker->executor->arena);
            ++stopped_receipts;
        }
        pthread_cond_broadcast(&boundary_condition);
        pthread_mutex_unlock(&boundary_mutex);
        return e;
    }
    assert(!input->error);
    if (input->operation == VCO_AGENT_STOPPED) {
        ++stopped_receipts;
        assert(!input->ticket && !input->status.count && !input->status.revision && !patch_live);
        assert(worker->executor->stopped && !worker->executor->arena);
        return stopped_delivery_error;
    }
    last = *input;
    ++publishes;
    if (publishes == 1) {
        assert(last.status.count == 2 && !game[0] && game[2] == 7 && !patch_live);
        assert(worker->executor->engine.rows[0].state == VCO_ENTRY_OFF &&
               worker->executor->engine.rows[1].state == VCO_ENTRY_OFF); /* _V1 never starts ON */
    }
    if (publishes == 3)
        assert(game[0] == 0x40 && game[2] == 0x99 && patch_live);
    if ((leave_on && publishes == 3) || publishes == 4)
        vco_game_worker_request_stop(worker);
    return 0;
}
static void run_thread(void)
{
    assert(worker && worker->started && !thread_ran);
    thread_ran = true;
    (void)thread_entry(sizeof(worker), &worker);
    assert(__atomic_load_n(&worker->completed, __ATOMIC_ACQUIRE));
}
static unsigned live_blocks(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < allocations; ++i)
        n += blocks[i].live;
    return n;
}
static void pool_capacity(void)
{
    VcoGamePool p = {0};
    assert(!vco_game_pool_init(&p));
    assert(vco_game_pool_init(&p) == VCO_E_ARGUMENT);
    VcoAgentAllocator a = vco_game_pool_allocator(&p);
    assert(!a.allocate(&p, 0) && !a.allocate(&p, SIZE_MAX));
    assert(!a.allocate(&p, VCO_GAME_ALLOCATION_MAX + 1));
    void *payload = a.allocate(&p, VCO_UPLOAD_MAX);
    void *current = a.allocate(&p, VCO_GAME_ALLOCATION_MAX);
    void *candidate = a.allocate(&p, VCO_GAME_ALLOCATION_MAX);
    assert(payload && current && candidate && !a.allocate(&p, 1) && allocations == 3);
    a.release(&p, current);
    assert(a.allocate(&p, 17) && allocations == 4 && !p.poisoned);
    assert(!vco_game_pool_dispose(&p) && !live_blocks() && frees == 4);
}
static void pool_faults(bool failed_free)
{
    VcoGamePool p = {0};
    assert(!vco_game_pool_init(&p));
    VcoAgentAllocator a = vco_game_pool_allocator(&p);
    alloc_error = -20;
    assert(!a.allocate(&p, 16) && !p.poisoned && !allocations);
    alloc_error = 0;
    base_error = -21;
    free_error = failed_free ? -22 : 0;
    assert(!a.allocate(&p, 16) && frees == 1);
    if (failed_free) {
        assert(p.poisoned && p.blocks[0].release_failed && live_blocks() == 1);
        assert(!a.allocate(&p, 16) && allocations == 1);
        assert(vco_game_pool_dispose(&p) == VCO_E_IO && frees == 1); /* no second SDK free */
    } else {
        assert(!live_blocks() && !p.poisoned);
        base_error = 0;
        assert(a.allocate(&p, 16) && !vco_game_pool_dispose(&p) && frees == 2);
    }
}
int taiHookFunctionExportForKernel(int32_t pid, tai_hook_ref_t *reference, const char *module,
                                   uint32_t library, uint32_t nid, const void *callback)
{
    (void)nid;
    assert(boundary_mode && pid == KERNEL_PID && !strcmp(module, "SceKernelModulemgr") &&
           library == 0xEAED1616 && boundary_hooks < 8);
    *reference = 200 + boundary_hooks;
    boundary_callbacks[boundary_hooks] =
        (int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))callback;
    return 100 + (int)boundary_hooks++;
}
void vco_agent_kernel_loader_failed(int error)
{
    (void)error;
    assert(!"unexpected module hook installation failure");
}
VcoAgentStatus vco_agent_kernel_module_begin(VcoModuleCall *out)
{
    *out = (VcoModuleCall){0};
    pthread_mutex_lock(&boundary_mutex);
    if (boundary_broker.quiescent) {
        pthread_mutex_unlock(&boundary_mutex);
        return VCO_AGENT_OK;
    }
    VcoAgentStatus error = vco_broker_module_begin(&boundary_broker, 77, out);
    VcoError cleanup = VCO_OK;
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    ++deadline.tv_sec; /* Test scheduler limit, not firmware waiting evidence. */
    while (!error && vco_broker_module_ready(&boundary_broker, out, &cleanup) == VCO_AGENT_EMPTY)
        assert(!pthread_cond_timedwait(&boundary_condition, &boundary_mutex, &deadline));
    if (!error && cleanup) {
        error = VCO_AGENT_IO;
        assert(!vco_broker_module_end(&boundary_broker, out));
        *out = (VcoModuleCall){0};
    }
    pthread_mutex_unlock(&boundary_mutex);
    return error;
}
VcoAgentStatus vco_agent_kernel_module_end(const VcoModuleCall *call)
{
    if (!call->claim)
        return VCO_AGENT_OK;
    pthread_mutex_lock(&boundary_mutex);
    VcoAgentStatus error = vco_broker_module_end(&boundary_broker, call);
    pthread_mutex_unlock(&boundary_mutex);
    return error;
}
void vco_agent_kernel_module_trace(unsigned operation, int32_t result, const VcoModuleCall *call)
{
    (void)result;
    assert(operation < 8 && call);
}
int entry_module_continue(tai_hook_ref_t ref, uintptr_t uid, uintptr_t b, uintptr_t c, uintptr_t d)
{
    assert(uid == 21 && !b && !c && !d);
    ++boundary_originals;
    if (ref == 204) {
        assert(boundary_module_stopped && worker->thread < 0);
        ++boundary_unloads;
        return 0;
    }
    assert(ref == 203 || ref == 205);
    int result = module_stop(0, NULL); /* Actual Game module entry. */
    if (result == SCE_KERNEL_STOP_CANCEL)
        return -200;
    assert(result == SCE_KERNEL_STOP_SUCCESS && worker->thread < 0);
    boundary_module_stopped = true;
    if (ref == 205)
        ++boundary_unloads;
    return 0;
}
static void boundary_lifecycle(const char *test)
{
    boundary_mode = true;
    restore_error = !strcmp(test, "boundary_restore_failure");
    assert(!vco_broker_init(&boundary_broker, 99, boundary_upload, sizeof(boundary_upload)));
    assert(!vco_broker_open(&boundary_broker, 77, "PCSB00031", &binding));
    assert(!vco_broker_observe(&boundary_broker, binding.session, true));
    assert(!vco_agent_modules_start() && boundary_hooks == 8);
    assert(module_start(sizeof(binding), &binding) == SCE_KERNEL_START_SUCCESS);
    pthread_mutex_lock(&boundary_mutex);
    boundary_launch = true;
    pthread_cond_broadcast(&boundary_condition);
    uint8_t bytes[1024];
    size_t length = packet(bytes);
    uint64_t ticket;
    assert(!vco_broker_load(&boundary_broker, 99, bytes, length, &ticket));
    while (!boundary_broker.status.revision)
        pthread_cond_wait(&boundary_condition, &boundary_mutex);
    VcoControlRequest command = {.request = 1,
                                 .session = binding.session,
                                 .revision = boundary_broker.status.revision,
                                 .abi = VCO_CONTROL_ABI,
                                 .size = sizeof(command),
                                 .operation = VCO_CONTROL_SET,
                                 .entry = 1,
                                 .enabled = 1,
                                 .count = 1};
    assert(!vco_broker_submit(&boundary_broker, 99, &command, &ticket));
    VcoAgentResult reply;
    while (vco_broker_receive(&boundary_broker, 99, ticket, &reply) == VCO_AGENT_EMPTY)
        pthread_cond_wait(&boundary_condition, &boundary_mutex);
    assert(!vco_broker_ack(&boundary_broker, 99, ticket));
    assert(patch_live && game[2] == 0x99);
    pthread_mutex_unlock(&boundary_mutex);
    unsigned index = !strcmp(test, "boundary_stop_unload") ? 5u : 3u;
    int first = boundary_callbacks[index](21, 0, 0, 0);
    if (restore_error) {
        assert(first == (int32_t)UINT32_C(0x8002D01D) && !boundary_originals);
        vco_game_worker_request_stop(worker);
    } else {
        assert(first == -200 && boundary_originals == 1); /* First stop requests thread exit. */
        pthread_mutex_lock(&boundary_mutex);
        while (!boundary_broker.quiescent)
            pthread_cond_wait(&boundary_condition, &boundary_mutex);
        assert(stopped_receipts == 1 && !patch_live && game[2] == 7 && restores == 1);
        pthread_mutex_unlock(&boundary_mutex);
    }
    for (unsigned i = 0; !__atomic_load_n(&worker->completed, __ATOMIC_ACQUIRE); ++i) {
        assert(i < 1000);
        struct timespec delay = {0, 1000000};
        assert(!nanosleep(&delay, NULL));
    }
    if (restore_error) {
        assert(!boundary_broker.quiescent && !stopped_receipts && restores == 1);
        assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL);
        return;
    }
    assert(boundary_callbacks[index](21, 0, 0, 0) == 0 && boundary_originals == 2);
    if (index == 3)
        assert(boundary_callbacks[4](21, 0, 0, 0) == 0);
    assert(boundary_unloads == 1 && !live_blocks() && waits == 1 && deletes == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    game[2] = 7;
    const char *test = argv[1];
    if (!strncmp(test, "boundary_", 9))
        boundary_lifecycle(test);
    else if (!strcmp(test, "pool_capacity"))
        pool_capacity();
    else if (!strcmp(test, "pool_faults"))
        pool_faults(false);
    else if (!strcmp(test, "pool_free_failure"))
        pool_faults(true);
    else {
        if (!strcmp(test, "invalid")) {
            assert(module_start(0, NULL) == SCE_KERNEL_START_FAILED && !allocations && !creates);
            assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
        }
        attach_busy = !strcmp(test, "attach_busy") ? 2u : 0u;
        attach_error = !strcmp(test, "attach")                   ? VCO_AGENT_ARGUMENT
                       : !strcmp(test, "attach_busy_exhaustion") ? VCO_AGENT_BUSY
                                                                 : 0;
        alloc_error = !strcmp(test, "alloc") ? -20 : 0;
        base_error = !strcmp(test, "base") ? -21 : 0;
        create_error = !strcmp(test, "create") ? -22 : 0;
        start_error = (!strcmp(test, "start") || !strcmp(test, "delete")) ? -23 : 0;
        delete_error = !strcmp(test, "delete") ? -24 : 0;
        revoked_mode = !strcmp(test, "revoked");
        fault_mode = !strcmp(test, "fault");
        stopped_delivery_error = !strcmp(test, "stopped_failure") ? VCO_AGENT_IO : 0;
        restore_error = !strcmp(test, "restore_failure");
        leave_on = !strcmp(test, "enabled_stop") || restore_error;
        int started = module_start(sizeof(binding), &binding);
        if (!strcmp(test, "attach_busy"))
            assert(attach_calls == 3 && startup_delays == 2);
        if (!strcmp(test, "attach_busy_exhaustion"))
            assert(attach_calls == 8 && startup_delays == 7 && !allocations && !creates);
        if (attach_error || alloc_error || base_error || create_error ||
            (start_error && !delete_error)) {
            assert(started == SCE_KERNEL_START_FAILED && !live_blocks() && !restores);
            assert(reports == 1 &&
                   reported == (alloc_error || base_error ? VCO_E_LIMIT : VCO_E_IO));
        } else if (delete_error) {
            assert(started == SCE_KERNEL_START_SUCCESS && reports == 1 && reported == VCO_E_IO);
            assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL && live_blocks() == 1);
            delete_error = 0;
            assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS && !live_blocks());
        } else {
            assert(started == SCE_KERNEL_START_SUCCESS && allocations == 1 && starts == 1);
            assert(module_start(sizeof(binding), &binding) == SCE_KERNEL_START_SUCCESS &&
                   starts == 1);
            if (!strcmp(test, "invalid")) {
                assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL && !waits && !frees);
            }
            run_thread();
            if (revoked_mode || restore_error) {
                assert(!stopped_receipts);
                assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL);
                assert(live_blocks() == 2 && restores == (restore_error ? 1u : 0u));
                assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL && restores <= 1);
                if (restore_error)
                    assert(reported == VCO_E_RESTORE);
                else
                    assert(!reports && patch_live && game[2] == 0x99);
            } else {
                if (!strcmp(test, "wait")) {
                    wait_error = -25;
                    assert(module_stop(0, NULL) == SCE_KERNEL_STOP_CANCEL && live_blocks() == 1);
                    wait_error = 0;
                }
                assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS && !live_blocks());
                assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
                assert(game[2] == 7 && restores == (!strcmp(test, "invalid") ? 0u : 1u));
                if (fault_mode)
                    assert(reports == 1 && reported == VCO_E_IO);
                assert(stopped_receipts == 1);
                if (stopped_delivery_error)
                    assert(reports == 1 && reported == VCO_E_IO);
            }
        }
    }
    /* Host process teardown only. It does not model SDK/taiHEN exit cleanup. */
    for (unsigned i = 0; i < allocations; ++i)
        if (blocks[i].base)
            free(blocks[i].base);
    printf("game entry %s passed\n", test);
    return 0;
}
