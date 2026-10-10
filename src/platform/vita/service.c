#include "service.h"
#include "close_service.h"
#include "../../execution/service_protocol.h"
#include "authority.h"
#include "control_rpc.h"
#include "shell_owner.h"
#include "patches.h"
#include "modules.h"
#include "pad.h"
#include "service_trace.h"
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
#include <string.h>

/* This module stays resident. The worker exclusively owns execution and native
 * resources until successful join/deletion and quiescence of its old callers.
 * Only then can authenticated Status syscalls serialize metadata retirement;
 * input hooks never access those resources or restore target bytes. */
typedef char status_size_check[sizeof(VcoServiceStatus) == 80 ? 1 : -1];
enum { LOAD_EMPTY, LOAD_COPYING, LOAD_READY, LOAD_CLEAR, LOAD_PROCESSING };
typedef struct {
    int32_t block;
    VcoCheatFile file;
    VcoCheatOp *ops;
    VcoPatch *patches;
    size_t capacity;
} RuntimeArena;
static VcoVitaShellOwner shell_owner;
static uint32_t owner_state, startup_state, gate, call_state, maintenance_gate, observer_exhausted;
#define CALL_CLOSED UINT32_C(0x80000000)
#define CALL_COUNT UINT32_C(0x7fffffff)
static VcoVitaGameAuthority authority;
static VcoVitaMemory native_memory;
static VcoVitaExecution execution;
static VcoVitaPatchSupport patch_support;
static VcoVitaModules modules;
static VcoVitaModuleRecord module_records[VCO_VITA_MODULE_LIMIT];
static VcoVitaPad pad;
static VcoSession session;
static VcoEngine engine;
static VcoControlMailbox mailbox;
static VcoCloseQueue close_queue;
static VcoVitaControlWorker worker;
static VcoVitaControlRpc rpc;
static RuntimeArena arena = {.block = -1};
/* Failed native release retains its allocation record for the resident
 * service's lifetime. cleanup_failed forbids replacing this record. */
static RuntimeArena quarantined_arena = {.block = -1};
static VcoServiceStatus published;
static VcoVitaCloseTarget published_close;
static VcoForegroundSample observation;
static uint64_t observation_time, observation_serial;
static uint32_t observation_epoch;
static void *upload_bytes;
static int32_t upload_block = -1;
static int32_t quarantined_block = -1;
static uint32_t upload_state, upload_length;
static uint64_t last_load, last_key, last_revision;
/* Serialized by upload_state, independently of best-effort status publication.
 * Includes rejected packets: their request is complete, never retried as new. */
static VcoUploadHeader last_processed;
static uint32_t catalog_revision;
static VcoError service_error, execution_fault;
static bool cleanup_failed, retired_cleanup_pending;
static bool owner_rebinding, restart_owner_released;
static uint64_t restart_mailbox_sequence;
/* Terminal backend only. This gate owns post-join metadata, independently of
 * the short publication gate. Each uncertain release is attempted only once. */
static uint32_t terminal_gate, terminal_error;
static bool terminal_upload_attempted, terminal_complete, terminal_native_poisoned;
/* Independent diagnostic ownership: never reset on backend/Shell restarts.
 * Native callbacks only append RAM records; user space owns all trace I/O. */
static VcoTraceRing native_trace;
static VcoServiceStatus traced_status;
static VcoForegroundSample traced_observation;
static int traced_observation_result;
static bool traced_status_valid, traced_observation_valid;
extern int vco_kernel_capturing(void);
extern int vco_kernel_input_ready(void);
extern uint32_t vco_kernel_capture_epoch(void);
extern void vco_kernel_capture_reset(void);
static void publish(void);
static void consume_process_events(void);

static void trace_native(void *unused, uint32_t event, int32_t result, uint32_t subject,
                         uint32_t detail)
{
    (void)unused;
    int64_t clock = ksceKernelGetSystemTimeWide();
    /* A long A group can emit more than the ring and file retain during its
     * cleanup. Preserve the first actual injection failure and preceding raw
     * calls. Execution/cleanup continue; this diagnostic candidate stops only
     * later trace appends until full reboot. No callback performs file I/O. */
    if (event == VCO_PATCH_TRACE_INJECT_FAILURE)
        (void)vco_trace_freeze(&native_trace, clock < 0 ? 0 : (uint64_t)clock, event, result,
                               subject, detail);
    else
        (void)vco_trace_append(&native_trace, clock < 0 ? 0 : (uint64_t)clock, event, result,
                               subject, detail);
}

static int take_gate(void)
{
    uint32_t free_gate = 0;
    return __atomic_compare_exchange_n(&gate, &free_gate, 1, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}
static void leave_gate(void)
{
    __atomic_store_n(&gate, 0, __ATOMIC_RELEASE);
}
static int enter_call(void)
{
    if (!vco_kernel_input_ready())
        return VCO_MAILBOX_CLOSED;
    /* Gate bit and reference count share one atomic: closing cannot miss a
     * concurrent successful admission on weakly ordered ARM cores. */
    uint32_t active = __atomic_load_n(&call_state, __ATOMIC_ACQUIRE);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if ((active & CALL_CLOSED) || active == CALL_COUNT)
            return VCO_MAILBOX_BUSY;
        if (__atomic_compare_exchange_n(&call_state, &active, active + 1, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return VCO_MAILBOX_OK;
    }
    return VCO_MAILBOX_BUSY;
}
void vco_service_call_end(void)
{
    __atomic_sub_fetch(&call_state, 1, __ATOMIC_RELEASE);
}
static int authorize_owner(int32_t caller)
{
    uint32_t state = __atomic_load_n(&owner_state, __ATOMIC_ACQUIRE);
    if (!state) {
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&owner_state, &expected, 1, false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED))
            return -1;
        /* Bind the actual Sysroot Shell, regardless of which process called. */
        vco_vita_shell_owner_init(&shell_owner);
        int result = vco_vita_shell_owner_bind(&shell_owner);
        state = result ? (shell_owner.poisoned ? 3u : 0u) : 2u;
        __atomic_store_n(&owner_state, state, __ATOMIC_RELEASE);
    }
    return state == 2 ? vco_vita_shell_owner_authorize(&shell_owner, caller) : -1;
}
/* Only the execution worker consumes observations, including from its native
 * authority callbacks. A long entry must not leave newer authenticated RPC
 * receipts queued until its entire enable/rollback round finishes. Copy once
 * under the short gate, then validate outside it; never renew receipt time. */
static void consume_observation(void)
{
    VcoForegroundSample sample;
    uint64_t serial = 0, received = 0;
    uint32_t epoch = 0;
    if (take_gate()) {
        if (observation_serial > authority.serial) {
            sample = observation;
            serial = observation_serial;
            received = observation_time;
            epoch = observation_epoch;
        }
        leave_gate();
    }
    if (!serial)
        return;
    /* A departed Shell cannot extend a game lease using an earlier queued
     * packet. Consume that serial as invalid so it cannot be replayed. */
    bool owner_valid = vco_kernel_input_ready() &&
                       __atomic_load_n(&owner_state, __ATOMIC_ACQUIRE) == 2 &&
                       !authorize_owner(shell_owner.pid);
    int accepted = vco_vita_authority_accept(&authority, owner_valid ? &sample : NULL,
                                             sizeof(sample), received, epoch, serial);
    if (!traced_observation_valid || accepted != traced_observation_result ||
        memcmp(&sample, &traced_observation, sizeof(sample))) {
        trace_native(NULL, VCO_SERVICE_TRACE_OBSERVE, accepted, sample.status,
                     (uint32_t)sample.game_pid);
        trace_native(NULL, VCO_SERVICE_TRACE_OBSERVE_META, (int32_t)sample.failure,
                     (uint32_t)sample.active_pid, epoch);
        trace_native(NULL, VCO_SERVICE_TRACE_OBSERVE_RECEIPT, sample.active_app_id,
                     (uint32_t)received, (uint32_t)(received >> 32));
        traced_observation = sample;
        traced_observation_result = accepted;
        traced_observation_valid = true;
    }
}
static int authorize_game(void *context, const VcoIdentity *identity, bool foreground)
{
    if (foreground) {
        if (execution_fault || cleanup_failed ||
            __atomic_load_n(&observer_exhausted, __ATOMIC_ACQUIRE)) {
            vco_vita_authority_revoke(&authority);
            return -1;
        }
        consume_observation();
    }
    return vco_vita_authority_authorize(context, identity, foreground);
}
static bool caller_maintenance_possible(uint32_t lifecycle)
{
    return lifecycle == 0 || lifecycle == 3 || lifecycle == 4 ||
           (lifecycle == 2 && !__atomic_load_n(&worker.accepting, __ATOMIC_ACQUIRE));
}
static void maintain_owner_without_worker(void)
{
    uint32_t lifecycle = __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE);
    if (!caller_maintenance_possible(lifecycle) ||
        __atomic_load_n(&call_state, __ATOMIC_ACQUIRE) != CALL_CLOSED)
        return;
    uint32_t free_gate = 0;
    if (!__atomic_compare_exchange_n(&maintenance_gate, &free_gate, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED))
        return;
    lifecycle = __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE);
    if (caller_maintenance_possible(lifecycle) &&
        __atomic_load_n(&call_state, __ATOMIC_ACQUIRE) == CALL_CLOSED) {
        if (lifecycle == 2) {
            VcoError cleanup = VCO_OK;
            /* accepting=0 only starts this probe. WaitThreadEnd + mailbox
             * completion + thread deletion establish the actual handoff.
             * This input-context path never touches engine/native resources. */
            if (vco_vita_control_worker_reap_stopped(&worker, &cleanup) || !take_gate())
                goto done;
            published = (VcoServiceStatus){.abi = VCO_SERVICE_ABI,
                                           .size = sizeof(published),
                                           .load_request = last_load,
                                           .state = VCO_SERVICE_ERROR,
                                           .error = cleanup           ? cleanup
                                                    : execution_fault ? execution_fault
                                                                      : VCO_E_IO};
            leave_gate();
            /* Terminal backend state: input owner may recover, execution may
             * never restart. A Status syscall can now own metadata retirement;
             * a native cleanup error still needs independent retirement proof. */
            __atomic_store_n(&startup_state, 4, __ATOMIC_RELEASE);
            trace_native(NULL, VCO_SERVICE_TRACE_WORKER_END, worker.run_error, (uint32_t)cleanup,
                         (uint32_t)shell_owner.pid);
        }
        uint32_t state = __atomic_load_n(&owner_state, __ATOMIC_ACQUIRE);
        int replacement = state == 2 ? vco_vita_shell_owner_replacement(&shell_owner) : -1;
        if (state == 2 && replacement == 0) {
            /* Also covers a prior reaper success followed by a temporary
             * owner lookup failure. Every reopening releases old capture. */
            vco_kernel_capture_reset();
            __atomic_store_n(&call_state, 0, __ATOMIC_RELEASE);
        }
        if (state == 2 && replacement == 1) {
            /* No live execution worker remains. Only the input channel is
             * rebound after all old helper scopes exited; old engine/native
             * ownership and permanent errors are deliberately untouched. */
            if (vco_vita_shell_owner_unbind(&shell_owner))
                state = 3;
            else {
                vco_vita_shell_owner_init(&shell_owner);
                state = 4;
            }
            __atomic_store_n(&owner_state, state, __ATOMIC_RELEASE);
        }
        if (state == 4) {
            if (!vco_vita_shell_owner_bind(&shell_owner)) {
                vco_kernel_capture_reset();
                __atomic_store_n(&owner_state, 2, __ATOMIC_RELEASE);
                __atomic_store_n(&call_state, 0, __ATOMIC_RELEASE);
            } else if (shell_owner.poisoned)
                __atomic_store_n(&owner_state, 3, __ATOMIC_RELEASE);
        }
    }
done:
    __atomic_store_n(&maintenance_gate, 0, __ATOMIC_RELEASE);
}
int vco_service_call_begin(int32_t caller)
{
    if (!vco_kernel_input_ready())
        return VCO_MAILBOX_CLOSED;
    int result = enter_call();
    if (result) {
        maintain_owner_without_worker();
        result = enter_call();
        if (result)
            return result;
    }
    if (authorize_owner(caller)) {
        uint32_t lifecycle = __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE);
        if (caller_maintenance_possible(lifecycle) &&
            __atomic_load_n(&owner_state, __ATOMIC_ACQUIRE) == 2 &&
            vco_vita_shell_owner_replacement(&shell_owner) == 1)
            __atomic_fetch_or(&call_state, CALL_CLOSED, __ATOMIC_ACQ_REL);
        vco_service_call_end();
        maintain_owner_without_worker();
        return VCO_MAILBOX_OWNER;
    }
    if (__atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) == 2 &&
        !__atomic_load_n(&worker.accepting, __ATOMIC_ACQUIRE)) {
        /* The same Shell should reap a stopped worker too. Relinquish this
         * entire scope before attempting the nonblocking lifecycle handoff. */
        __atomic_fetch_or(&call_state, CALL_CLOSED, __ATOMIC_ACQ_REL);
        vco_service_call_end();
        maintain_owner_without_worker();
        return VCO_MAILBOX_BUSY;
    }
    return VCO_MAILBOX_OK;
}
int vco_service_authorize_caller(int32_t caller)
{
    if (vco_service_call_begin(caller))
        return -1;
    vco_service_call_end();
    return 0;
}
static int user_range(const void *p, uint32_t length)
{
    uintptr_t start = (uintptr_t)p;
    return length && start >= 0x40000000u && start <= UINT32_MAX &&
           (uint64_t)start + length <= UINT64_C(0x100000000);
}
static int allocate(const char *name, size_t size, void **base)
{
    size = (size + 4095u) & ~(size_t)4095u;
    int block = ksceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, size, NULL);
    if (block < 0)
        return block;
    if (ksceKernelGetMemBlockBase(block, base) < 0 || !*base) {
        if (ksceKernelFreeMemBlock(block) < 0) {
            quarantined_block = block;
            cleanup_failed = true;
            retired_cleanup_pending = false;
            service_error = VCO_E_IO;
        }
        return -1;
    }
    memset(*base, 0, size);
    return block;
}
static int capture_sample(void *unused, uint32_t *epoch)
{
    (void)unused;
    uint32_t before = vco_kernel_capture_epoch();
    int active = vco_kernel_capturing();
    uint32_t after = vco_kernel_capture_epoch();
    if (before != after)
        return -1;
    *epoch = after;
    return active;
}
static int discard_arena(RuntimeArena *a)
{
    if (a->block >= 0 && ksceKernelFreeMemBlock(a->block) < 0)
        return -1;
    memset(a, 0, sizeof(*a));
    a->block = -1;
    return 0;
}
static VcoError decode_arena(RuntimeArena *a, VcoUploadHeader *header)
{
    VcoUploadHeader hint;
    memcpy(&hint, upload_bytes, sizeof(hint));
    if (!vco_upload_size(hint.entries, hint.codes) ||
        vco_upload_size(hint.entries, hint.codes) != upload_length)
        return VCO_E_ARGUMENT;
    unsigned entries = hint.entries ? hint.entries : 1;
    unsigned codes = hint.codes ? hint.codes : 1;
    size_t bytes = entries * sizeof(VcoCheatEntry) + codes * sizeof(VcoCode) +
                   codes * sizeof(VcoCheatOp) + codes * sizeof(VcoPatch);
    void *base = NULL;
    a->block = allocate("vco_code_arena", bytes, &base);
    if (a->block < 0)
        return cleanup_failed ? service_error : VCO_E_LIMIT;
    a->file.entries = base;
    a->file.entry_capacity = entries;
    a->file.codes = (VcoCode *)(a->file.entries + entries);
    a->file.code_capacity = codes;
    a->ops = (VcoCheatOp *)(a->file.codes + codes);
    a->patches = (VcoPatch *)(a->ops + codes);
    a->capacity = codes;
    return vco_upload_decode(upload_bytes, upload_length, &a->file, header);
}
static VcoError close_native_list(void *context)
{
    return vco_vita_execution_dispose(context) ? VCO_E_RESTORE : VCO_OK;
}
static VcoError close_target_on_worker(void *unused, const VcoEngineCloseKey *key,
                                       VcoEngineCloseResult *result)
{
    (void)unused;
    if (key->scope != VCO_CLOSE_PROCESS && key->scope != VCO_CLOSE_MODULE)
        return VCO_E_ARGUMENT;
    if (!native_memory.bound || key->process_object != (uintptr_t)native_memory.process_object)
        return VCO_E_SESSION;
    if (key->scope == VCO_CLOSE_MODULE) {
        bool found = false;
        if (!modules.mapped || modules.generation != key->module_generation ||
            modules.session != key->session)
            return VCO_E_SESSION;
        for (unsigned i = 0; i < modules.count; ++i)
            if (modules.records[i].uid == key->module_uid)
                found = true;
        if (!found)
            return VCO_E_SESSION;
        /* The boundary producer, not this metadata sample, holds/verifies the
         * original module object and native mapping protection. Its opaque tag
         * participates in exact request/result identity, never as a lifetime pin. */
    }
    VcoError error = vco_engine_close(&engine, key, close_native_list, &execution, result);
    if (engine.closing && engine.close_complete &&
        (engine.close_result.entries || engine.close_result.native)) {
        cleanup_failed = retired_cleanup_pending = true;
        service_error = VCO_E_RESTORE;
    }
    publish();
    return error;
}
static void detach_engine(void)
{
    uint64_t last_close_transaction = engine.last_close_transaction;
    vco_engine_init(&engine);
    engine.last_revision = last_revision;
    engine.last_close_transaction = last_close_transaction;
}
static void finish_target_on_worker(void *unused, const VcoCloseReply *reply)
{
    (void)unused;
    const VcoEngineCloseKey *key = &reply->key;
    /* Process termination remains closed until independently trusted retirement
     * permits detachment/rebinding. A failed close cannot reopen any scope. */
    if (key->scope != VCO_CLOSE_MODULE || reply->error || reply->cleanup.entries ||
        reply->cleanup.native || cleanup_failed || execution_fault)
        return;
    if (!engine.bound || !engine.closing || !engine.close_complete ||
        !vco_close_key_equal(key, &engine.close_key))
        return;
    VcoError error = VCO_OK;
    consume_process_events();
    if ((vco_process_events_invalidation() & VCO_PROCESS_EVENTS_LOST) ||
        __atomic_load_n(&observer_exhausted, __ATOMIC_ACQUIRE)) {
        execution_fault = VCO_E_SESSION;
        goto done;
    }
    if (!native_memory.bound || key->process_object != (uintptr_t)native_memory.process_object ||
        native_memory.poisoned || authority.retired ||
        vco_vita_authority_authorize(&authority, &key->identity, false)) {
        error = VCO_E_SESSION;
        goto done;
    }
    if (vco_vita_execution_error(&execution) == VCO_E_RESTORE) {
        cleanup_failed = retired_cleanup_pending = true;
        error = VCO_E_RESTORE;
        goto done;
    }
    if (last_revision == UINT64_MAX) {
        error = VCO_E_LIMIT;
        goto done;
    }
    /* Original module tags are only compared; they can already be retired.
     * Rebuild from current SDK metadata using this backend's own retained
     * process reference. No old byte restoration or automatic enable occurs. */
    uint64_t generation;
    unsigned count;
    if (vco_vita_modules_snapshot(&modules, key->session, &generation, &count) ||
        vco_vita_modules_map_z06(&modules, key->session, generation)) {
        error = VCO_E_METADATA;
        goto done;
    }
    /* Metadata queries can outlast a terminal event or object replacement.
     * Resample both before reopening; a successful snapshot is not a lease. */
    consume_process_events();
    if ((vco_process_events_invalidation() & VCO_PROCESS_EVENTS_LOST) ||
        __atomic_load_n(&observer_exhausted, __ATOMIC_ACQUIRE)) {
        execution_fault = VCO_E_SESSION;
        goto done;
    }
    if (!vco_vita_control_worker_running(&worker))
        goto done; /* Stop may race already-admitted metadata work. */
    if (vco_vita_authority_authorize(&authority, &key->identity, false)) {
        error = VCO_E_SESSION;
        goto done;
    }
    if (vco_vita_execution_rearm(&execution)) {
        cleanup_failed = true;
        retired_cleanup_pending = false;
        error = VCO_E_SESSION;
        goto done;
    }
    error = vco_engine_reopen(&engine, key, last_revision + 1);
    if (!error)
        last_revision = engine.revision;
done:
    if (error)
        service_error = error;
    publish();
}
static int clear_list(void)
{
    VcoError result;
    if (engine.bound && (engine.closing || engine.last_close_transaction != UINT64_MAX)) {
        VcoEngineCloseKey key =
            engine.closing
                ? engine.close_key
                : (VcoEngineCloseKey){.identity = engine.identity,
                                      .session = engine.token,
                                      .revision = engine.revision,
                                      .transaction = engine.last_close_transaction + 1,
                                      .scope = VCO_CLOSE_LIST,
                                      .process_object = (uintptr_t)native_memory.process_object};
        result = vco_engine_close(&engine, &key, close_native_list, &execution, NULL);
        if (!result)
            result = vco_engine_unbind(&engine);
    } else {
        /* No list, or exhausted transaction numbering: ordinary cleanup still
         * runs. This fallback never publishes a new scoped close confirmation. */
        result = vco_engine_unbind(&engine);
        VcoError native = close_native_list(&execution);
        if (!result)
            result = native;
    }
    if (result != VCO_OK) {
        cleanup_failed = true;
        retired_cleanup_pending = true;
        service_error = VCO_E_RESTORE;
        return -1;
    }
    if (discard_arena(&arena)) {
        cleanup_failed = true;
        retired_cleanup_pending = false;
        service_error = VCO_E_IO;
        return -1;
    }
    catalog_revision = 0;
    last_key = 0;
    return 0;
}
static int release_target_references(void)
{
    uint32_t pid = (uint32_t)native_memory.identity.pid;
    int memory_result = vco_vita_memory_unbind(&native_memory);
    trace_native(NULL, VCO_SERVICE_TRACE_TARGET_MEMORY_UNBIND, memory_result, pid, 0);
    /* Distinct owners hold distinct references. An ambiguous/poisoned memory
     * release must not skip the authority owner's still-known release. Neither
     * release is retried here, and either error still isolates execution. */
    int authority_result = vco_vita_authority_unbind(&authority);
    trace_native(NULL, VCO_SERVICE_TRACE_TARGET_AUTHORITY_UNBIND, authority_result, pid, 0);
    return memory_result || authority_result ? -1 : 0;
}
static int clear_target(void)
{
    uint32_t pid = (uint32_t)native_memory.identity.pid;
    trace_native(NULL, VCO_SERVICE_TRACE_TARGET_CLEAR_BEGIN, 0, pid, authority.retired);
    if (authority.retired) {
        /* Process exit may already have closed taiHEN's native handles. Never
         * write old addresses or forgive a live/ambiguous handle. The reaper
         * releases only independently proven closed handle references. */
        int result = vco_vita_execution_reap_retired(&execution, &native_memory.identity);
        trace_native(NULL, VCO_SERVICE_TRACE_TARGET_REAP, result, pid, 0);
        if (result) {
            cleanup_failed = true;
            retired_cleanup_pending = result > 0;
            service_error = VCO_E_RESTORE;
            return -1;
        }
        detach_engine();
        cleanup_failed = retired_cleanup_pending = false;
    }
    int result = clear_list();
    trace_native(NULL, VCO_SERVICE_TRACE_TARGET_LIST_CLEAR, result, pid, 0);
    if (result)
        return -1;
    vco_vita_modules_invalidate(&modules);
    if (release_target_references()) {
        cleanup_failed = true;
        retired_cleanup_pending = false;
        service_error = VCO_E_SESSION;
        return -1;
    }
    vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL);
    service_error = VCO_OK;
    trace_native(NULL, VCO_SERVICE_TRACE_TARGET_CLEAR_END, 0, pid, 0);
    return 0;
}
static void reclaim_retired_target(bool verify_identity)
{
    if (!native_memory.bound)
        return;
    if (!authority.retired && (verify_identity || (cleanup_failed && retired_cleanup_pending)))
        /* This independent check may establish an object/title replacement.
         * Its return is never permission to execute, and a lookup error is
         * never retirement. Neither it nor retired reaping accesses old bytes. */
        (void)vco_vita_authority_authorize(&authority, &native_memory.identity, false);
    if (authority.poisoned) {
        /* An ambiguous temporary-reference release is a permanent authority
         * fault, even if this check ran without a foreground memory operation.
         * Never hide it as background/UNKNOWN or rebind a replacement target.
         * Independent, later retirement can still release other known owners. */
        execution_fault = VCO_E_SESSION;
        vco_vita_authority_revoke(&authority);
    }
    if (!authority.retired)
        return;
    if (!cleanup_failed ||
        (retired_cleanup_pending && quarantined_arena.block < 0 && quarantined_block < 0))
        /* A known retired target can progress without Shell ownership. Keep
         * ambiguous native/arena failures isolated; a live original handle
         * permits only one bounded reaping attempt per worker update. */
        (void)clear_target();
}
static void consume_process_events(void)
{
    VcoProcessEvent event;
    for (unsigned i = 0; i < VCO_PROCESS_EVENTS_CAPACITY; ++i) {
        if (vco_process_events_poll(&event) != VCO_PROCESS_EVENTS_OK)
            break;
        trace_native(NULL, VCO_SERVICE_TRACE_PROCESS, event.pid, event.kind, event.invalidation);
        trace_native(NULL, VCO_SERVICE_TRACE_PROCESS_META, event.event_type,
                     (uint32_t)event.event_id, event.flags);
        trace_native(NULL, VCO_SERVICE_TRACE_PROCESS_PARAM, event.parameter_pid,
                     event.parameter_size, event.invalidation);
        vco_vita_authority_event(&authority, &event);
    }
}
static void terminal_fault(VcoError error)
{
    VcoError prior = __atomic_load_n(&terminal_error, __ATOMIC_RELAXED);
    if (!prior || error == VCO_E_SESSION || (prior == VCO_E_IO && error == VCO_E_RESTORE))
        __atomic_store_n(&terminal_error, error, __ATOMIC_RELEASE);
}
static int maintain_terminal_target(void)
{
    if (__atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) != 4)
        return VCO_MAILBOX_OK;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&terminal_gate, &expected, 1, false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        return VCO_MAILBOX_BUSY;
    /* startup_state=4 was published only after full old-call quiescence and
     * successful WaitThreadEnd, mailbox completion, and DeleteThread. Other
     * terminal RPCs cannot load/observe/execute. This syscall's caller scope
     * prevents concurrent Shell rebind; terminal_gate serializes Status peers.
     * No publication/owner gate is held across SDK calls. */
    if (!terminal_upload_attempted) {
        terminal_upload_attempted = true;
        __atomic_store_n(&upload_state, LOAD_EMPTY, __ATOMIC_RELEASE);
        if (upload_block >= 0) {
            if (ksceKernelFreeMemBlock(upload_block) < 0)
                terminal_fault(VCO_E_IO); /* Keep the record; never retry its UID. */
            else {
                upload_block = -1;
                upload_bytes = NULL;
            }
        }
    }
    if (terminal_complete || terminal_native_poisoned)
        goto done;
    if (worker.cleanup != VCO_OK) {
        /* Failed engine/native finalization owns live receipts. Only bounded
         * identity/UID queries are allowed; never call unbind/dispose again or
         * touch old target bytes from this replacement owner. */
        consume_process_events();
        if (!native_memory.bound)
            goto done; /* No independently verifiable binding: retain it. */
        if (!authority.retired)
            (void)vco_vita_authority_authorize(&authority, &native_memory.identity, false);
        if (authority.poisoned)
            terminal_fault(VCO_E_SESSION);
        if (!authority.retired)
            goto done;
        int result = vco_vita_execution_reap_retired(&execution, &native_memory.identity);
        trace_native(NULL, VCO_SERVICE_TRACE_TARGET_REAP, result,
                     (uint32_t)native_memory.identity.pid, 0);
        if (result) {
            if (result < 0) {
                terminal_native_poisoned = true;
                terminal_fault(VCO_E_RESTORE);
            }
            goto done; /* Original UID live: one poll per valid Status call. */
        }
    }
    /* Successful worker finalization OR retired native reaping proves that no
     * native receipt owns these engine buffers. Detach without executing an
     * engine step or restoring any byte. Releases below are distinct owners;
     * a failure must not skip the others, and none may be repeated. */
    detach_engine();
    if (discard_arena(&arena))
        terminal_fault(VCO_E_IO);
    vco_vita_modules_invalidate(&modules);
    if (release_target_references())
        terminal_fault(VCO_E_SESSION);
    vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL);
    catalog_revision = 0;
    last_key = 0;
    terminal_complete = true;
done:
    __atomic_store_n(&terminal_gate, 0, __ATOMIC_RELEASE);
    return VCO_MAILBOX_OK;
}
static int bind_target(int32_t pid, const char title[10])
{
    if (vco_vita_authority_prepare(&authority)) {
        if (authority.poisoned) {
            cleanup_failed = true;
            service_error = VCO_E_SESSION;
        }
        return -1;
    }
    VcoIdentity identity;
    if (vco_vita_memory_bind(&native_memory, pid, title, &identity))
        goto failed;
    vco_session_observe(&session, vco_vita_authority_observation(&authority), &identity);
    if (!session.present)
        goto failed;
    vco_vita_execution_init(&execution, &native_memory, &session, &patch_support);
    VcoVitaExecutionFeatures features = {0};
    int module_result =
        vco_vita_modules_init(&modules, &execution.data, module_records, VCO_VITA_MODULE_LIMIT);
    vco_vita_modules_set_trace(&modules, trace_native, NULL);
    if (!module_result) {
        features.module_context = &modules;
        features.module = vco_vita_modules_resolve;
        features.module_guard = vco_vita_modules_guard;
    }
    if (vco_vita_pad_init(&pad, &execution.data, capture_sample, NULL)) {
        features.pad_context = &pad;
        features.pad = vco_vita_pad_read;
    }
    if (!vco_vita_execution_set_features(&execution, features))
        return 0;
failed:
    /* No engine has borrowed this target yet. Still unwind both references
     * when authority expired/replaced between bind and session observation.
     * The executor may still describe the previous (already disposed) target;
     * do not pass the new identity to that old executor's retired reaper. */
    vco_vita_modules_invalidate(&modules);
    if (release_target_references()) {
        cleanup_failed = true;
        retired_cleanup_pending = false;
        service_error = VCO_E_SESSION;
    } else
        vco_session_observe(&session, VCO_OBSERVATION_ABSENT, NULL);
    return -1;
}
static bool maintain_owner(void)
{
    if (owner_rebinding && __atomic_load_n(&owner_state, __ATOMIC_ACQUIRE) == 3)
        return false;
    if (!owner_rebinding) {
        if (!vco_vita_shell_owner_authorize(&shell_owner, shell_owner.pid))
            return true;
        vco_vita_authority_revoke(&authority);
        /* A temporary lookup failure is not proof of a Shell restart. */
        if (vco_vita_shell_owner_replacement(&shell_owner) != 1)
            return false;
        __atomic_fetch_or(&call_state, CALL_CLOSED, __ATOMIC_ACQ_REL);
        owner_rebinding = true;
    }
    if (__atomic_load_n(&call_state, __ATOMIC_ACQUIRE) & CALL_COUNT)
        return false;
    if (!restart_owner_released) {
        /* Entire old syscalls/input helpers are now quiescent. No new caller
         * can observe owner/mailbox mutation until admission reopens. */
        if (!cleanup_failed && native_memory.bound)
            (void)clear_target();
        vco_session_observe(&session, VCO_OBSERVATION_UNKNOWN, NULL);
        __atomic_store_n(&upload_state, LOAD_EMPTY, __ATOMIC_RELEASE);
        observation_serial = authority.serial;
        restart_mailbox_sequence = mailbox.sequence;
        if (vco_vita_shell_owner_unbind(&shell_owner)) {
            __atomic_store_n(&owner_state, 3, __ATOMIC_RELEASE);
            service_error = VCO_E_SESSION;
            return false;
        }
        __atomic_store_n(&owner_state, 4, __ATOMIC_RELEASE);
        vco_vita_shell_owner_init(&shell_owner);
        restart_owner_released = true;
    }
    if (vco_vita_shell_owner_bind(&shell_owner)) {
        if (shell_owner.poisoned) {
            __atomic_store_n(&owner_state, 3, __ATOMIC_RELEASE);
            service_error = VCO_E_SESSION;
        }
        return false;
    }
    vco_mailbox_init(&mailbox, &engine, shell_owner.pid);
    /* Never reuse a ticket from the departed Shell, even after its old
     * mailbox request/reply was intentionally discarded. */
    mailbox.sequence = restart_mailbox_sequence;
    rpc =
        (VcoVitaControlRpc){&worker, shell_owner.pid, vco_vita_shell_owner_authorize, &shell_owner};
    vco_kernel_capture_reset();
    owner_rebinding = restart_owner_released = false;
    __atomic_store_n(&owner_state, 2, __ATOMIC_RELEASE);
    __atomic_store_n(&call_state, 0, __ATOMIC_RELEASE);
    return false; /* Only a new Shell observation can authorize another tick. */
}
static void prepare_idle_modules(void *unused, uint64_t token)
{
    (void)unused;
    if (!execution.features.module || vco_vita_execution_error(&execution))
        return;
    /* A failed native injection can retain ownership without publishing a
     * runtime receipt. Engine all-OFF alone is insufficient to replace stamps. */
    for (unsigned i = 0; i < VCO_VITA_PATCH_LIMIT; ++i) {
        const VcoVitaPatch *p = &execution.patches[i];
        if (p->active || p->quarantined || p->referenced || p->block_referenced || p->object ||
            p->block_object || p->tai_handle > 0 || p->memblock > 0)
            return;
    }
    vco_vita_modules_prepare_idle(&modules, token);
}
static void consume_load(void)
{
    uint32_t state = __atomic_load_n(&upload_state, __ATOMIC_ACQUIRE);
    if (state != LOAD_READY && state != LOAD_CLEAR)
        return;
    __atomic_store_n(&upload_state, LOAD_PROCESSING, __ATOMIC_RELEASE);
    if (state == LOAD_CLEAR) {
        if (!cleanup_failed && !clear_list())
            service_error = VCO_OK;
        __atomic_store_n(&upload_state, LOAD_EMPTY, __ATOMIC_RELEASE);
        return;
    }
    RuntimeArena next = {.block = -1};
    VcoUploadHeader header;
    memcpy(&header, upload_bytes, sizeof(header));
    last_processed = header;
    last_load = header.request;
    VcoError result = execution_fault  ? execution_fault
                      : cleanup_failed ? service_error
                                       : decode_arena(&next, &header);
    if (!result) {
        if (!session.present || header.target_session != session.token ||
            header.pid != (uint32_t)session.identity.pid ||
            memcmp(header.title_id, session.identity.title_id, 10))
            result = VCO_E_SESSION;
        else if (clear_list())
            result = service_error;
        else if (vco_vita_execution_rearm(&execution)) {
            cleanup_failed = true;
            retired_cleanup_pending = false;
            result = VCO_E_SESSION;
        } else if (last_revision == UINT64_MAX)
            result = VCO_E_LIMIT;
        else {
            /* Prime the all-OFF list. Games may load additional modules after
             * this point; the first idle manual ON will take a fresh snapshot. */
            uint64_t generation;
            unsigned count;
            if (execution.features.module &&
                !vco_vita_modules_snapshot(&modules, session.token, &generation, &count))
                (void)vco_vita_modules_map_z06(&modules, session.token, generation);
            result = vco_engine_bind(&engine, &session, ++last_revision, &next.file,
                                     vco_vita_execution_callbacks(&execution), next.ops,
                                     next.patches, next.capacity);
            if (!result) {
                result = vco_engine_set_idle_prepare(&engine, prepare_idle_modules, NULL);
                if (result)
                    (void)vco_engine_unbind(&engine);
            }
            if (!result) {
                arena = next;
                next.block = -1;
                last_key = header.content_key;
                catalog_revision = header.catalog_revision;
            }
        }
    }
    if (discard_arena(&next)) {
        quarantined_arena = next;
        cleanup_failed = true;
        retired_cleanup_pending = false;
        result = VCO_E_IO;
    }
    /* Receipt metadata describes the last processed request even on failure;
     * revision/count/catalog_revision still describe the actually bound list. */
    last_key = last_processed.content_key;
    service_error = result;
    __atomic_store_n(&upload_state, LOAD_EMPTY, __ATOMIC_RELEASE);
}
static void publish(void)
{
    VcoServiceStatus status = {0};
    status.abi = VCO_SERVICE_ABI;
    status.size = sizeof(status);
    status.session = session.present ? session.token : 0;
    status.revision = engine.bound ? engine.revision : 0;
    status.load_request = last_load;
    status.content_key = last_key;
    VcoError native_error = vco_vita_execution_error(&execution);
    /* A failed injection may retain a private native handle that never entered
     * the engine's public patch list. OFF replies do not prove that handle was
     * restored: publish the native failure even if no row owns a public key.
     * A cleared empty executor can still be rearmed by the next all-OFF upload. */
    if (native_error == VCO_E_PATCH && !engine.bound)
        native_error = VCO_OK;
    status.error = execution_fault ? execution_fault : service_error ? service_error : native_error;
    status.count = engine.bound ? engine.count : 0;
    status.catalog_revision = catalog_revision;
    status.event_epoch = vco_process_events_invalidation();
    status.pid = session.present ? session.identity.pid : 0;
    if (session.present)
        memcpy(status.title_id, session.identity.title_id, 10);
    status.state = execution_fault     ? VCO_SERVICE_ERROR
                   : status.error      ? VCO_SERVICE_ERROR
                   : !session.present  ? VCO_SERVICE_NO_GAME
                   : !engine.bound     ? VCO_SERVICE_LOADING
                   : !session.runnable ? VCO_SERVICE_BACKGROUND
                                       : VCO_SERVICE_READY;
    if (!traced_status_valid || status.state != traced_status.state ||
        status.error != traced_status.error || status.event_epoch != traced_status.event_epoch)
        trace_native(NULL, VCO_SERVICE_TRACE_STATE, (int32_t)status.error, status.state,
                     status.event_epoch);
    if (!traced_status_valid || status.pid != traced_status.pid ||
        status.session != traced_status.session)
        trace_native(NULL, VCO_SERVICE_TRACE_SESSION, status.pid, (uint32_t)status.session,
                     (uint32_t)(status.session >> 32));
    if (!traced_status_valid || status.revision != traced_status.revision)
        trace_native(NULL, VCO_SERVICE_TRACE_LIST, (int32_t)status.count, (uint32_t)status.revision,
                     (uint32_t)(status.revision >> 32));
    if (status.load_request != traced_status.load_request)
        trace_native(NULL, VCO_SERVICE_TRACE_UPLOAD, (int32_t)status.error,
                     (uint32_t)status.load_request, (uint32_t)(status.load_request >> 32));
    traced_status = status;
    traced_status_valid = true;
    if (take_gate()) {
        published = status;
        published_close = (VcoVitaCloseTarget){0};
        if (engine.bound && native_memory.bound && engine.last_close_transaction != UINT64_MAX) {
            published_close.key =
                engine.closing ? engine.close_key
                               : (VcoEngineCloseKey){
                                     .identity = engine.identity,
                                     .session = engine.token,
                                     .revision = engine.revision,
                                     .transaction = engine.last_close_transaction + 1,
                                     .scope = VCO_CLOSE_PROCESS,
                                     .process_object = (uintptr_t)native_memory.process_object};
            published_close.module_snapshot = modules.generation;
        }
        leave_gate();
    }
}
static void update(void *unused, uint64_t now)
{
    (void)unused;
    (void)now;
    if (!patch_support.attempted) {
        (void)vco_vita_patch_support_probe(&patch_support);
        if (vco_vita_patch_support_dispose(&patch_support)) {
            cleanup_failed = true;
            service_error = VCO_E_PATCH;
        }
    }
    consume_process_events();
    consume_observation();
    if ((vco_process_events_invalidation() & VCO_PROCESS_EVENTS_LOST) ||
        __atomic_load_n(&observer_exhausted, __ATOMIC_ACQUIRE))
        execution_fault = VCO_E_SESSION;
    if (execution_fault)
        vco_vita_authority_revoke(&authority);
    /* Reclamation belongs to this worker even when Shell identity checks or
     * input-owner rebinding fail. Revoked execution stays revoked; this only
     * discards resources of an independently proven retired game. */
    reclaim_retired_target(false);
    /* A departed/replaced Shell must not keep a still-unexpired observation
     * alive. Consume its queued serial but revoke it before any engine work. */
    if (!maintain_owner()) {
        /* Missing Shell ownership alone is not retirement evidence. Check the
         * retained game independently for replacement, including a lost EXIT.
         * Already-retired/pending targets were attempted above: do not poll
         * them again, or repeat an ambiguous release, within this update. */
        if (!authority.retired)
            reclaim_retired_target(true);
        vco_session_observe(&session, VCO_OBSERVATION_UNKNOWN, NULL);
        publish();
        return;
    }
    int32_t pid = 0;
    char title[10] = {0};
    int target = vco_vita_authority_target(&authority, &pid, title);
    if (!cleanup_failed && !execution_fault) {
        bool changed = native_memory.bound &&
                       (!target && (pid != native_memory.identity.pid ||
                                    memcmp(title, native_memory.identity.title_id, 10)));
        if (native_memory.bound && (authority.retired || changed))
            (void)clear_target();
        if (!cleanup_failed && !target && !native_memory.bound) {
            if (bind_target(pid, title)) {
                if (!cleanup_failed)
                    service_error = VCO_E_SESSION;
            } else
                service_error = VCO_OK;
        }
        vco_session_observe(&session, vco_vita_authority_observation(&authority),
                            &native_memory.identity);
    } else {
        vco_session_observe(&session, VCO_OBSERVATION_UNKNOWN, NULL);
    }
    consume_load();
    publish();
}
static VcoError finalize_execution(void *unused)
{
    (void)unused;
    /* Engine unbind only sees published receipts. The native backend also owns
     * failed-injection receipts which must be finalized by this same worker. */
    if (vco_vita_execution_dispose(&execution)) {
        cleanup_failed = true;
        service_error = VCO_E_RESTORE;
        return VCO_E_RESTORE;
    }
    return VCO_OK;
}
static int ensure_started(int32_t caller)
{
    if (authorize_owner(caller))
        return VCO_MAILBOX_OWNER;
    uint32_t state = __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE);
    if (state == 2)
        return __atomic_load_n(&worker.accepting, __ATOMIC_ACQUIRE) ? VCO_MAILBOX_OK
                                                                    : VCO_MAILBOX_CLOSED;
    if (state == 3 || state == 4)
        return VCO_MAILBOX_CLOSED;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&startup_state, &expected, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED))
        return VCO_MAILBOX_BUSY;
    vco_vita_authority_init(&authority);
    vco_vita_memory_init(&native_memory, authorize_game, &authority);
    (void)vco_vita_memory_set_trace(&native_memory, trace_native, NULL);
    vco_session_init(&session);
    vco_engine_init(&engine);
    vco_vita_patch_support_init(&patch_support);
    (void)vco_vita_patch_support_set_trace(&patch_support, trace_native, NULL);
    vco_vita_execution_init(&execution, &native_memory, &session, &patch_support);
    vco_mailbox_init(&mailbox, &engine, shell_owner.pid);
    vco_vita_control_worker_init(&worker);
    vco_close_queue_init(&close_queue);
    worker.close_queue = &close_queue;
    worker.close_execute = close_target_on_worker;
    worker.close_finish = finish_target_on_worker;
    worker.finalize = finalize_execution;
    rpc =
        (VcoVitaControlRpc){&worker, shell_owner.pid, vco_vita_shell_owner_authorize, &shell_owner};
    published = (VcoServiceStatus){
        .abi = VCO_SERVICE_ABI, .size = sizeof(published), .state = VCO_SERVICE_STARTING};
    upload_block = allocate("vco_upload", VCO_UPLOAD_MAX, &upload_bytes);
    int result = upload_block < 0 ? -1 : vco_process_events_start();
    if (!result)
        result = vco_vita_control_worker_start(&worker, &mailbox, update, NULL);
    __atomic_store_n(&startup_state, result ? 3u : 2u, __ATOMIC_RELEASE);
    trace_native(NULL, VCO_SERVICE_TRACE_START, result, (uint32_t)shell_owner.pid,
                 result ? 3u : 2u);
    return result ? VCO_MAILBOX_CLOSED : VCO_MAILBOX_OK;
}
VcoCloseStatus vco_service_close_target(VcoVitaCloseTarget *out)
{
    if (!out)
        return VCO_CLOSE_ARGUMENT;
    if (__atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) != 2)
        return VCO_CLOSE_CLOSED;
    if (!take_gate())
        return VCO_CLOSE_BUSY;
    VcoVitaCloseTarget copy = published_close;
    leave_gate();
    if (!vco_close_key_valid(&copy.key))
        return VCO_CLOSE_WAIT;
    *out = copy;
    return VCO_CLOSE_OK;
}
VcoCloseStatus vco_service_close_submit(uint64_t requester, const VcoEngineCloseKey *key,
                                        uint64_t *ticket)
{
    if (__atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) != 2)
        return VCO_CLOSE_CLOSED;
    return vco_vita_control_worker_close_submit(&worker, requester, key, ticket);
}
VcoCloseStatus vco_service_close_receive(uint64_t requester, uint64_t ticket,
                                         VcoVitaCloseResult *out)
{
    uint32_t state = __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE);
    if (state != 2 && state != 4)
        return VCO_CLOSE_CLOSED;
    return vco_vita_control_worker_close_receive(&worker, requester, ticket, out);
}
VcoCloseStatus vco_service_close_release(uint64_t requester, uint64_t ticket)
{
    uint32_t state = __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE);
    if (state != 2 && state != 4)
        return VCO_CLOSE_CLOSED;
    return vco_vita_control_worker_close_release(&worker, requester, ticket);
}
int vcoServiceTrace(uint64_t after, VcoTraceBatch *out, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    int result = vco_service_call_begin(caller);
    bool scoped = !result;
    if (!result) {
        if (length != sizeof(*out) || !user_range(out, length))
            result = VCO_MAILBOX_ARGUMENT;
        else {
            VcoTraceBatch copy;
            VcoTraceResult snapshot = vco_trace_snapshot(&native_trace, after, &copy);
            if (snapshot != VCO_TRACE_OK)
                result = snapshot == VCO_TRACE_BUSY ? VCO_MAILBOX_BUSY : VCO_MAILBOX_ARGUMENT;
            else if (authorize_owner(caller))
                result = VCO_MAILBOX_OWNER;
            else if (ksceKernelCopyToUser(out, &copy, sizeof(copy)) < 0)
                result = VCO_MAILBOX_COPY;
        }
    }
    if (scoped)
        vco_service_call_end();
    EXIT_SYSCALL(state);
    return result;
}
int vcoServiceStatus(VcoServiceStatus *out, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    int result = vco_service_call_begin(caller);
    bool scoped = !result;
    if (!result)
        result = ensure_started(caller);
    if (scoped && result == VCO_MAILBOX_CLOSED &&
        __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) == 4)
        result = VCO_MAILBOX_OK; /* Execution stays closed; metadata may retire. */
    if (!result) {
        if (length != sizeof(*out) || !user_range(out, length))
            result = VCO_MAILBOX_ARGUMENT;
        else if (__atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) == 4 && authorize_owner(caller))
            result = VCO_MAILBOX_OWNER;
        else if ((result = maintain_terminal_target()) != VCO_MAILBOX_OK) {
            /* Another terminal Status owns metadata. No wait in this syscall. */
        } else if (!take_gate())
            result = VCO_MAILBOX_BUSY;
        else {
            VcoServiceStatus copy = published;
            leave_gate();
            VcoError cleanup = __atomic_load_n(&terminal_error, __ATOMIC_ACQUIRE);
            if (cleanup)
                copy.error = cleanup;
            if (authorize_owner(caller))
                result = VCO_MAILBOX_OWNER;
            else if (ksceKernelCopyToUser(out, &copy, sizeof(copy)) < 0)
                result = VCO_MAILBOX_COPY;
        }
    }
    if (scoped)
        vco_service_call_end();
    EXIT_SYSCALL(state);
    return result;
}
int vcoServiceObserve(const VcoForegroundSample *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    int result = vco_service_call_begin(caller);
    bool scoped = !result;
    if (!result)
        result = ensure_started(caller);
    if (!result) {
        VcoForegroundSample sample;
        if (length != sizeof(sample) || !user_range(input, length))
            result = VCO_MAILBOX_ARGUMENT;
        else if (ksceKernelCopyFromUser(&sample, input, length) < 0)
            result = VCO_MAILBOX_COPY;
        else if (authorize_owner(caller))
            result = VCO_MAILBOX_OWNER;
        else {
            /* Capture at receipt; waiting in a queue never renews authority. */
            int64_t received = ksceKernelGetSystemTimeWide();
            uint32_t epoch = vco_process_events_invalidation();
            if (received < 0)
                result = VCO_MAILBOX_CLOSED;
            else if (!take_gate())
                result = VCO_MAILBOX_BUSY;
            else {
                if (observation_serial == UINT64_MAX) {
                    __atomic_store_n(&observer_exhausted, 1, __ATOMIC_RELEASE);
                    result = VCO_MAILBOX_EXHAUSTED;
                } else {
                    observation = sample;
                    observation_time = (uint64_t)received;
                    observation_epoch = epoch;
                    ++observation_serial;
                }
                leave_gate();
            }
        }
    }
    if (scoped)
        vco_service_call_end();
    EXIT_SYSCALL(state);
    return result;
}
static int admit_load_session(uint64_t target_session)
{
    if (!target_session)
        return VCO_MAILBOX_ARGUMENT;
    if (!take_gate())
        return VCO_MAILBOX_BUSY;
    /* Syscalls may only read the published copy, never the worker's session.
     * A receipt can outlive this observation; consume_load checks again. */
    bool matches = published.session && target_session == published.session;
    leave_gate();
    return matches ? VCO_MAILBOX_OK : VCO_MAILBOX_ARGUMENT;
}
int vcoServiceLoad(const void *input, uint32_t length)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    int32_t caller = ksceKernelGetProcessId();
    int result = vco_service_call_begin(caller);
    bool scoped = !result;
    if (!result)
        result = ensure_started(caller);
    if (!result) {
        bool clear = !input && !length;
        VcoUploadHeader header;
        if (!clear &&
            (length < sizeof(header) || length > VCO_UPLOAD_MAX || !user_range(input, length)))
            result = VCO_MAILBOX_ARGUMENT;
        else if (!clear && ksceKernelCopyFromUser(&header, input, sizeof(header)) < 0)
            result = VCO_MAILBOX_COPY;
        else if (!clear && (header.size != length || header.abi != VCO_UPLOAD_ABI ||
                            !header.request || !header.content_key))
            result = VCO_MAILBOX_ARGUMENT;
        else if (authorize_owner(caller))
            result = VCO_MAILBOX_OWNER;
        else if (!clear && (result = admit_load_session(header.target_session)) != VCO_MAILBOX_OK) {
            /* Preserve the existing list and upload receipt on stale admission. */
        } else {
            uint32_t empty = LOAD_EMPTY;
            if (!__atomic_compare_exchange_n(&upload_state, &empty, LOAD_COPYING, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                result = VCO_MAILBOX_BUSY;
            else if (clear)
                __atomic_store_n(&upload_state, LOAD_CLEAR, __ATOMIC_RELEASE);
            else {
                /* A worker-processed retry is acknowledged without binding a
                 * second list. Request numbers are global for this boot. */
                if (header.request < last_processed.request ||
                    (header.request == last_processed.request &&
                     memcmp(&header, &last_processed, sizeof(header))))
                    result = VCO_MAILBOX_ARGUMENT;
                else if (header.request == last_processed.request)
                    result = VCO_MAILBOX_EMPTY;
                if (!result && ksceKernelCopyFromUser(upload_bytes, input, length) < 0)
                    result = VCO_MAILBOX_COPY;
                /* A caller may mutate its header while the payload is copied.
                 * Process only the exact envelope admitted above. */
                if (!result && memcmp(&header, upload_bytes, sizeof(header)))
                    result = VCO_MAILBOX_ARGUMENT;
                if ((!result || result == VCO_MAILBOX_EMPTY) && authorize_owner(caller))
                    result = VCO_MAILBOX_OWNER;
                if (!result) {
                    upload_length = length;
                    __atomic_store_n(&upload_state, LOAD_READY, __ATOMIC_RELEASE);
                } else {
                    __atomic_store_n(&upload_state, LOAD_EMPTY, __ATOMIC_RELEASE);
                    if (result == VCO_MAILBOX_EMPTY)
                        result = VCO_MAILBOX_OK;
                }
            }
        }
    }
    if (scoped)
        vco_service_call_end();
    EXIT_SYSCALL(state);
    return result;
}
int vcoServiceSubmit(const VcoControlRequest *input, uint32_t length, uint64_t *ticket)
{
    int result = enter_call();
    if (result)
        return result;
    result =
        vco_vita_rpc_submit(__atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) == 2 ? &rpc : NULL,
                            input, length, ticket);
    vco_service_call_end();
    return result;
}
int vcoServiceReceive(uint64_t ticket, VcoControlReply *out, uint32_t length)
{
    int result = enter_call();
    if (result)
        return result;
    result = vco_vita_rpc_receive(
        __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) == 2 ? &rpc : NULL, ticket, out, length);
    vco_service_call_end();
    return result;
}
int vcoServiceAck(uint64_t ticket)
{
    int result = enter_call();
    if (result)
        return result;
    result = vco_vita_rpc_ack(__atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) == 2 ? &rpc : NULL,
                              ticket);
    vco_service_call_end();
    return result;
}
int vcoServiceResume(void *out, uint32_t length)
{
    int result = enter_call();
    if (result)
        return result;
    result = vco_vita_rpc_resume(
        __atomic_load_n(&startup_state, __ATOMIC_ACQUIRE) == 2 ? &rpc : NULL, out, length);
    vco_service_call_end();
    return result;
}
