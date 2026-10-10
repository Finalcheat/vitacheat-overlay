#include "fakes.h"
#include "platform/vita/authority.h"
#include "platform/vita/service.h"
#include "platform/vita/control_worker.h"
#include "platform/vita/patches.h"
#include "platform/vita/modules.h"
#include "platform/vita/pad.h"
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Real service, authority, Shell owner, RPC, upload, compiler, engine and
 * mailbox are linked. Only the SDK, scheduling and native transfer/patch
 * providers are substituted; those providers have separate native tests. */
ServiceFake fake;
static const VcoVitaPatchSupport *trace_support;
void service_fake_trace(uint32_t event, int32_t result, uint32_t subject, uint32_t detail)
{
    assert(trace_support && trace_support->trace);
    trace_support->trace(trace_support->trace_context, event, result, subject, detail);
}
static SceClass process_class;
static SceObjectBase objects[4], replacement;
static void *blocks[32];
static unsigned next_block;
static VcoVitaControlWorker *running;
static VcoVitaMemory *memory;
static VcoVitaExecution *native_execution;
static VcoProcessEvent queued_event;
static bool pending_event, game_replaced;
static int32_t watched_pid;
static bool terminal_seen;
static int slot(int32_t pid)
{
    assert(pid == TEST_GAME || pid == TEST_GAME + 1 || pid == TEST_SHELL || pid == TEST_SHELL + 1);
    return pid >= TEST_SHELL ? pid - TEST_SHELL + 2 : pid - TEST_GAME;
}
static const char *title(int32_t pid)
{
    return pid == TEST_GAME ? "PCSB00031" : "PCSB00032";
}
void service_fake_setup(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.input_ready = true;
    fake.caller = fake.shell_pid = TEST_SHELL;
    fake.now = 1000000;
    fake.epoch = 1;
    fake.phase = VCO_PROCESS_EVENTS_NEW;
    fake.module_uid = 17;
}
uint32_t fake_enter_syscall(void)
{
    assert(!fake.depth);
    ++fake.entered;
    ++fake.depth;
    return 0x1234;
}
void fake_exit_syscall(uint32_t state)
{
    assert(fake.depth == 1 && state == 0x1234);
    ++fake.exited;
    --fake.depth;
}
int32_t ksceKernelGetProcessId(void)
{
    assert(fake.depth);
    return fake.caller;
}
int32_t ksceKernelSysrootGetShellPid(void)
{
    return fake.shell_pid;
}
int64_t ksceKernelGetSystemTimeWide(void)
{
    return fake.now;
}
static unsigned char *user_pointer(const void *p, size_t length)
{
    uintptr_t address = (uintptr_t)p;
    assert(fake.depth && address >= TEST_BASE && address + length <= TEST_BASE + sizeof(fake.user));
    return fake.user + (address - TEST_BASE);
}
int ksceKernelCopyFromUser(void *out, const void *in, size_t size)
{
    unsigned call = ++fake.from_calls;
    if (call == fake.mutate_header_call) {
        VcoUploadHeader header;
        memcpy(&header, user_pointer(in, size), sizeof(header));
        ++header.request;
        memcpy(user_pointer(in, size), &header, sizeof(header));
    }
    memcpy(out, user_pointer(in, size), call == fake.fail_from_call ? size / 2 : size);
    if (call == fake.revoke_owner_call)
        fake.owner_replaced = true;
    if (call == fake.restart_during_copy) {
        /* Deterministic scheduler interleaving: the execution worker runs
         * while the syscall owns its copied envelope and full-call scope. */
        fake.owner_replaced = true;
        unsigned depth = fake.depth;
        fake.depth = 0;
        service_fake_tick();
        fake.depth = depth;
    }
    if (call == fake.replace_game_during_copy) {
        unsigned depth = fake.depth;
        fake.depth = 0;
        service_fake_replace_game();
        for (unsigned i = 0; i < 2; ++i) {
            VcoForegroundSample sample = service_fake_sample(TEST_GAME, VCO_FOREGROUND_CANDIDATE);
            memcpy(fake.user + TEST_SAMPLE, &sample, sizeof(sample));
            assert(!vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(sample)));
            service_fake_tick();
        }
        fake.depth = depth;
    }
    return call == fake.fail_from_call ? -1 : 0;
}
int ksceKernelCopyToUser(void *out, const void *in, size_t size)
{
    ++fake.to_calls;
    memcpy(user_pointer(out, size), in, fake.fail_to ? size / 2 : size);
    return fake.fail_to ? -1 : 0;
}
int ksceKernelAllocMemBlock(const char *name, unsigned type, size_t size, void *options)
{
    assert(name && type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW && !options && !(size % 4096));
    if (++fake.allocations == fake.fail_allocation_call)
        return -1;
    assert(++next_block < sizeof(blocks) / sizeof(*blocks));
    blocks[next_block] = calloc(1, size);
    assert(blocks[next_block]);
    ++fake.live_blocks;
    return (int)next_block;
}
int ksceKernelGetMemBlockBase(int block, void **base)
{
    assert(block > 0 && (unsigned)block <= next_block);
    if (fake.fail_base)
        return -1;
    *base = blocks[block];
    return 0;
}
int ksceKernelFreeMemBlock(int block)
{
    assert(block > 0 && (unsigned)block <= next_block && blocks[block]);
    ++fake.frees;
    if (fake.status_during_free) {
        fake.status_during_free = false;
        unsigned depth = fake.depth;
        fake.depth = 0;
        assert(vcoServiceStatus(TEST_USER(TEST_STATUS), sizeof(VcoServiceStatus)) ==
               VCO_MAILBOX_BUSY);
        fake.depth = depth;
    }
    if (fake.fail_free && !fake.consume_failed_free)
        return -1;
    free(blocks[block]);
    blocks[block] = NULL;
    --fake.live_blocks;
    return fake.fail_free ? -1 : 0;
}
static SceClass *get_class(void)
{
    return &process_class;
}
static int get_title(int32_t pid, char *out, uint32_t size)
{
    assert(pid != TEST_SHELL && size == 32);
    memset(out, 0, size);
    memcpy(out, title(pid), 10);
    return fake.fail_title ? -1 : 0;
}
int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *address)
{
    assert(pid == KERNEL_PID);
    *address = 0;
    if (!strcmp(module, "SceProcessmgr")) {
        assert((library == 0x7A69DE86 && function == 0xC6820972) ||
               (library == 0xEB1F8EF7 && function == 0x98AE4BC8));
        *address = (uintptr_t)get_class;
    } else {
        assert(!strcmp(module, "SceSysmem") && library == 0x3691DA45 && function == 0xEC3124A3);
        *address = (uintptr_t)get_title;
    }
    return 0;
}
int ksceGUIDReferObjectWithClass(SceUID pid, SceClass *cls, SceObjectBase **out)
{
    assert(cls == &process_class);
    if (pid == fake.shell_pid && fake.owner_lookup_failure)
        return -109;
    int i = slot(pid);
    __atomic_add_fetch(&fake.refs[i], 1, __ATOMIC_RELAXED);
    *out = (pid == TEST_SHELL && fake.owner_replaced) || (pid == TEST_GAME && game_replaced)
               ? &replacement
               : &objects[i];
    return 0;
}
int ksceGUIDReleaseObject(SceUID pid)
{
    int i = slot(pid);
    int error = pid == TEST_GAME ? fake.game_release_error : 0;
    if (error) {
        fake.game_release_error = 0;
        if (!fake.consume_failed_game_release)
            return error;
    }
    assert(__atomic_fetch_sub(&fake.refs[i], 1, __ATOMIC_RELAXED) > 0);
    return error;
}
int taiGetModuleInfoForKernel(int32_t pid, const char *name, tai_module_info_t *out)
{
    assert((pid == KERNEL_PID && !strcmp(name, "PSVitaCheatKernel")) ||
           (pid > 0 && !strcmp(name, "vitacheat")));
    assert(out->size == sizeof(*out));
    return fake.collision ? 0 : fake.collision_unknown ? -1 : (int32_t)0x90010002u;
}
int vco_process_events_start(void)
{
    if (fake.fail_events)
        return -1;
    fake.phase = VCO_PROCESS_EVENTS_ACTIVE;
    return 0;
}
uint32_t vco_process_events_invalidation(void)
{
    return fake.epoch;
}
void vco_process_events_state(VcoProcessEventsState *state)
{
    *state = (VcoProcessEventsState){fake.phase, fake.epoch, 0, 0};
}
int vco_process_events_watch(int32_t pid, uint32_t epoch)
{
    if (watched_pid || epoch != fake.epoch || (epoch & VCO_PROCESS_EVENTS_LOST) ||
        fake.phase != VCO_PROCESS_EVENTS_ACTIVE)
        return -1;
    watched_pid = pid;
    terminal_seen = false;
    return 0;
}
void vco_process_events_unwatch(int32_t pid)
{
    if (pid == watched_pid)
        watched_pid = 0;
}
int vco_process_events_allow(int32_t pid)
{
    return pid == watched_pid && !terminal_seen && fake.phase == VCO_PROCESS_EVENTS_ACTIVE;
}
VcoProcessEventsPoll vco_process_events_poll(VcoProcessEvent *event)
{
    if (!pending_event)
        return VCO_PROCESS_EVENTS_EMPTY;
    *event = queued_event;
    pending_event = false;
    return VCO_PROCESS_EVENTS_OK;
}
void service_fake_event(uint32_t kind, int32_t pid)
{
    assert(!pending_event);
    if (pid == watched_pid && (kind == VCO_PROCESS_EVENT_EXIT || kind == VCO_PROCESS_EVENT_KILL))
        terminal_seen = true;
    queued_event = (VcoProcessEvent){.invalidation = ++fake.epoch, .kind = kind, .pid = pid};
    pending_event = true;
}
void service_fake_replace_game(void)
{
    game_replaced = !game_replaced;
}
VcoForegroundSample service_fake_sample(int32_t pid, uint32_t status)
{
    VcoForegroundSample sample = {
        .abi = VCO_FOREGROUND_PROBE_ABI, .size = sizeof(sample), .status = status};
    if (status) {
        sample.game_pid = pid;
        sample.game_app_id = 22;
        sample.active_pid = status == VCO_FOREGROUND_CANDIDATE ? pid : TEST_SHELL;
        sample.active_app_id = status == VCO_FOREGROUND_CANDIDATE ? 22 : 11;
        memcpy(sample.game_title_id, title(pid), 10);
    }
    return sample;
}
void vco_vita_control_worker_init(VcoVitaControlWorker *w)
{
    memset(w, 0, sizeof(*w));
    w->thread = -1;
}
int vco_vita_control_worker_start(VcoVitaControlWorker *w, VcoControlMailbox *m,
                                  VcoVitaControlUpdate update, void *context)
{
    if (fake.fail_worker)
        return -1;
    assert(!running);
    w->mailbox = m;
    w->update = update;
    w->context = context;
    w->accepting = 1;
    w->started = true;
    w->thread = 123;
    running = w;
    return 0;
}
void service_fake_tick(void)
{
    assert(running && !fake.depth);
    bool parked = vco_close_queue_pump(running->close_queue, running->close_execute,
                                       running->close_finish, running->context);
    if (!parked)
        running->update(running->context, (uint64_t)fake.now);
    if (!parked)
        parked = vco_close_queue_pump(running->close_queue, running->close_execute,
                                      running->close_finish, running->context);
    if (!parked)
        assert(vco_mailbox_pump(running->mailbox, (uint64_t)fake.now));
}
int vco_vita_control_worker_running(const VcoVitaControlWorker *w)
{
    return w && w->accepting;
}
int vco_vita_control_worker_shutdown(const VcoVitaControlWorker *w, VcoVitaControlShutdown *out)
{
    if (w->accepting || fake.worker_pending)
        return 0;
    *out = (VcoVitaControlShutdown){.run_error = w->run_error, .cleanup = w->finalize_error};
    out->mailbox_complete = vco_mailbox_stop_result(w->mailbox, &out->cleanup) != 0;
    if (!out->cleanup)
        out->cleanup = w->finalize_error;
    return out->mailbox_complete;
}
void service_fake_worker_fault(void)
{
    assert(running && running->accepting);
    running->accepting = 0;
    running->run_error = -106;
    fake.worker_pending = true;
    vco_mailbox_stop(running->mailbox);
}
void service_fake_worker_finish(void)
{
    assert(running && fake.worker_pending);
    if (fake.worker_cleanup_failure)
        running->mailbox->engine->blocked_restore = true;
    assert(!vco_mailbox_pump(running->mailbox, (uint64_t)fake.now));
    if (running->finalize)
        running->finalize_error = running->finalize(running->context);
    fake.worker_pending = false;
}
int vco_vita_control_worker_reap_stopped(VcoVitaControlWorker *w, VcoError *cleanup)
{
    assert(w == running && !w->accepting);
    ++fake.worker_reaps;
    if (!w->joined) {
        ++fake.worker_waits;
        if (fake.worker_pending)
            return -107;
        w->joined = true;
        w->completed = vco_mailbox_stop_result(w->mailbox, &w->cleanup) != 0;
        if (w->cleanup == VCO_OK)
            w->cleanup = w->finalize_error;
    }
    *cleanup = w->cleanup;
    if (w->thread >= 0) {
        ++fake.worker_deletes;
        if (fake.worker_delete_failure)
            return -108;
        w->thread = -1;
    }
    return w->completed ? 0 : -1;
}
VcoMailboxStatus vco_vita_control_worker_submit(VcoVitaControlWorker *w, int32_t pid,
                                                const void *packet, size_t size, uint64_t *ticket)
{
    assert(w == running && fake.depth);
    return vco_mailbox_submit(w->mailbox, pid, packet, size, ticket);
}
VcoMailboxStatus vco_vita_control_worker_receive(VcoVitaControlWorker *w, int32_t pid,
                                                 uint64_t ticket, VcoControlReply *reply)
{
    assert(w == running && fake.depth);
    return vco_mailbox_receive(w->mailbox, pid, ticket, reply);
}
VcoMailboxStatus vco_vita_control_worker_ack(VcoVitaControlWorker *w, int32_t pid, uint64_t ticket)
{
    assert(w == running && fake.depth);
    return vco_mailbox_ack(w->mailbox, pid, ticket);
}
VcoMailboxStatus vco_vita_control_worker_resume(VcoVitaControlWorker *w, int32_t pid,
                                                VcoControlRequest *request, uint64_t *ticket)
{
    assert(w == running && fake.depth);
    return vco_mailbox_resume(w->mailbox, pid, request, ticket);
}
void vco_vita_memory_init(VcoVitaMemory *m, VcoVitaAuthority authorize, void *context)
{
    memset(m, 0, sizeof(*m));
    m->authority = authorize;
    m->authority_context = context;
    memory = m;
}
int vco_vita_memory_set_trace(VcoVitaMemory *m, VcoVitaMemoryTrace callback, void *context)
{
    assert(m == memory && !m->bound);
    m->trace = callback;
    m->trace_context = callback ? context : NULL;
    return 0;
}
int vco_vita_memory_bind(VcoVitaMemory *m, int32_t pid, const char id[10], VcoIdentity *out)
{
    assert(m == memory && !m->bound);
    ++fake.binds;
    if (fake.fail_bind)
        return -1;
    VcoIdentity identity = {pid, ++m->sequence, {0}};
    memcpy(identity.title_id, id, 10);
    if (m->authority(m->authority_context, &identity, false))
        return -1;
    m->identity = *out = identity;
    m->process_object = game_replaced && pid == TEST_GAME ? &replacement : &objects[slot(pid)];
    m->bound = true;
    if (fake.expire_bind) {
        fake.expire_bind = false;
        fake.now += VCO_VITA_AUTHORITY_LEASE_US;
    }
    if (fake.replace_bind) {
        fake.replace_bind = false;
        service_fake_replace_game();
    }
    return 0;
}
int vco_vita_memory_unbind(VcoVitaMemory *m)
{
    assert(m == memory && !m->locked);
    if (m->bound) {
        ++fake.unbinds;
        m->bound = false;
        m->process_object = NULL;
    }
    return fake.fail_memory_unbind ? -1 : 0;
}
void vco_vita_patch_support_init(VcoVitaPatchSupport *support)
{
    memset(support, 0, sizeof(*support));
}
int vco_vita_patch_support_set_trace(VcoVitaPatchSupport *support, VcoVitaPatchTrace callback,
                                     void *context)
{
    if (!support || support->attempted)
        return -1;
    support->trace = callback;
    support->trace_context = callback ? context : NULL;
    return 0;
}
int vco_vita_patch_support_probe(VcoVitaPatchSupport *support)
{
    trace_support = support;
    assert(!fake.depth && !support->attempted);
    support->attempted = true;
    support->ready = !fake.fail_patch_support;
    support->quarantined = fake.fail_patch_support;
    /* Transport evidence from a provider fake, not a real GUID probe. */
    if (support->trace)
        support->trace(support->trace_context, VCO_PATCH_TRACE_PROBE_RESULT,
                       fake.fail_patch_support ? -1 : 0, 0, support->ready ? 1 : 2);
    return fake.fail_patch_support ? -1 : 0;
}
int vco_vita_patch_support_dispose(const VcoVitaPatchSupport *support)
{
    return support->quarantined ? -1 : 0;
}
void vco_vita_execution_init(VcoVitaExecution *execution, VcoVitaMemory *m, const VcoSession *s,
                             const VcoVitaPatchSupport *support)
{
    assert(m == memory);
    memset(execution, 0, sizeof(*execution));
    execution->data.session = s;
    execution->data.transport.context = m;
    native_execution = execution;
    execution->identity = m->identity;
    execution->support = support;
}
int vco_vita_execution_set_features(VcoVitaExecution *e, VcoVitaExecutionFeatures features)
{
    assert(!e->sealed);
    if (fake.fail_features)
        return -1;
    e->features = features;
    return 0;
}
static int alive(void *context, uint64_t token)
{
    VcoVitaExecution *e = context;
    return !e->blocked && memory->bound && vco_session_matches(e->data.session, token, true) &&
           !memory->authority(memory->authority_context, &memory->identity, true);
}
static int write_game(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                      size_t length)
{
    assert(address == 0x81001000 && length == sizeof(fake.value));
    if (!alive(context, token))
        return -1;
    if (fake.block_during_write) {
        ((VcoVitaExecution *)context)->blocked = true;
        return -1;
    }
    memcpy(&fake.value, bytes, length);
    ++fake.writes;
    if (fake.write_delay_us) {
        /* Interleave the existing observer RPC with a slow execution round.
         * Receipt time is the RPC's kernel clock, never a worker refresh. */
        fake.now += fake.write_delay_us;
        if (fake.observe_during_write) {
            uint32_t kind = VCO_FOREGROUND_CANDIDATE;
            if (fake.observe_during_write == 2 && fake.writes >= 3)
                kind = VCO_BACKGROUND_CANDIDATE;
            if (fake.observe_during_write == 4 && fake.writes >= 3)
                kind = VCO_FOREGROUND_UNKNOWN;
            VcoForegroundSample sample = service_fake_sample(TEST_GAME, kind);
            memcpy(fake.user + TEST_SAMPLE, &sample, sizeof(sample));
            assert(!vcoServiceObserve(TEST_USER(TEST_SAMPLE), sizeof(sample)));
            if (fake.observe_during_write == 3)
                fake.now += VCO_VITA_AUTHORITY_LEASE_US;
        }
    }
    if (fake.expire_during_write) {
        fake.now += VCO_VITA_AUTHORITY_LEASE_US;
        assert(!alive(context, token));
        return VCO_ACCESS_SESSION;
    }
    return 0;
}
VcoCheatMemory vco_vita_execution_callbacks(VcoVitaExecution *e)
{
    e->sealed = true;
    return (VcoCheatMemory){.context = e, .alive = alive, .write = write_game};
}
int vco_vita_execution_dispose(VcoVitaExecution *e)
{
    ++fake.dispose_calls;
    if (fake.fail_dispose) {
        e->blocked = true;
        return -1;
    }
    return 0;
}
void service_fake_quarantine(void)
{
    assert(native_execution && memory->bound);
    native_execution->blocked = true;
    native_execution->patches[VCO_VITA_PATCH_LIMIT - 1].quarantined = true;
    fake.fail_dispose = true; /* Native residue cannot be cleared by an engine OFF. */
}
int vco_vita_execution_rearm(VcoVitaExecution *e)
{
    assert(!fake.depth && memory->bound && !fake.fail_dispose);
    ++fake.rearms;
    if (fake.fail_rearm || e->poisoned || e->retired)
        return -1;
    e->blocked = false;
    return 0;
}
int vco_vita_execution_reap_retired(VcoVitaExecution *e, const VcoIdentity *identity)
{
    assert(identity->instance == e->identity.instance && identity->pid == e->identity.pid);
    ++fake.reap_calls;
    if (!fake.reap_result) {
        e->retired = true;
        memset(e->patches, 0, sizeof(e->patches));
        fake.fail_dispose = false;
    }
    return fake.reap_result;
}
int vco_vita_modules_init(VcoVitaModules *m, const VcoMemoryAdapter *data,
                          VcoVitaModuleRecord *records, unsigned capacity)
{
    memset(m, 0, sizeof(*m));
    m->data = data;
    m->records = records;
    m->capacity = capacity;
    return 0;
}
void vco_vita_modules_invalidate(VcoVitaModules *m)
{
    m->mapped = false;
}
int vco_vita_modules_snapshot(VcoVitaModules *m, uint64_t token, uint64_t *generation,
                              unsigned *count)
{
    assert(vco_session_matches(m->data->session, token, false));
    ++fake.modules_snapshots;
    m->mapped = false;
    if (fake.fail_module_snapshot)
        return -1;
    if (fake.module_snapshot_action == 1)
        service_fake_replace_game();
    if (fake.module_snapshot_action == 2)
        service_fake_event(VCO_PROCESS_EVENT_EXIT, TEST_GAME);
    if (fake.module_snapshot_action == 3)
        fake.epoch |= VCO_PROCESS_EVENTS_LOST;
    *generation = ++m->generation;
    m->session = token;
    m->count = 1;
    m->records[0].uid = fake.module_uid;
    *count = 1;
    return 0;
}
int vco_vita_modules_map_z06(VcoVitaModules *m, uint64_t token, uint64_t generation)
{
    assert(generation == m->generation && vco_session_matches(m->data->session, token, false));
    ++fake.module_maps;
    if (fake.fail_module_map)
        return -1;
    m->mapped = true;
    return 0;
}
void vco_vita_modules_prepare_idle(void *context, uint64_t token)
{
    VcoVitaModules *m = context;
    uint64_t generation;
    unsigned count;
    assert(!fake.depth); /* Metadata preparation belongs to the execution worker. */
    assert(!vco_vita_modules_snapshot(m, token, &generation, &count));
    assert(!vco_vita_modules_map_z06(m, token, generation));
}
void service_fake_patch_residue(unsigned kind)
{
    assert(native_execution && memory->bound && kind <= 8);
    VcoVitaPatch *p = &native_execution->patches[VCO_VITA_PATCH_LIMIT - 1];
    memset(p, 0, sizeof(*p));
    switch (kind) {
    case 1:
        p->active = true;
        break;
    case 2:
        p->referenced = true;
        break;
    case 3:
        p->block_referenced = true;
        break;
    case 4:
        p->object = p;
        break;
    case 5:
        p->block_object = p;
        break;
    case 6:
        p->tai_handle = 123;
        break;
    case 7:
        p->memblock = 456;
        break;
    case 8:
        p->quarantined = true;
        break;
    default:
        break;
    }
}
int vco_vita_modules_resolve(void *ctx, uint64_t token, unsigned index, unsigned segment,
                             uint32_t *base, uint32_t *size)
{
    (void)ctx;
    (void)token;
    (void)index;
    (void)segment;
    (void)base;
    (void)size;
    return -1;
}
int vco_vita_modules_guard(void *ctx, uint64_t token, uint32_t address, uint32_t size,
                           uint64_t *generation)
{
    VcoVitaModules *m = ctx;
    (void)address;
    (void)size;
    if (!m->mapped || !vco_session_matches(m->data->session, token, false) ||
        (*generation && *generation != m->generation))
        return -1;
    *generation = m->generation;
    return 0;
}
bool vco_vita_pad_init(VcoVitaPad *pad, const VcoMemoryAdapter *data,
                       VcoVitaPadSuppression suppression, void *context)
{
    (void)pad;
    (void)data;
    (void)suppression;
    (void)context;
    return false;
}
int vco_vita_pad_read(void *ctx, uint64_t token, uint32_t *device, uint32_t *buttons)
{
    (void)ctx;
    (void)token;
    (void)device;
    (void)buttons;
    return -1;
}
int vco_kernel_input_ready(void)
{
    return fake.input_ready;
}
int vco_kernel_capturing(void)
{
    return 0;
}
uint32_t vco_kernel_capture_epoch(void)
{
    return 1;
}
void vco_kernel_capture_reset(void)
{
    ++fake.capture_resets;
}
