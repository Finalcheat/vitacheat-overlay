#include "process_events.h"
#include <psp2kern/kernel/proc_event.h>
#include <stddef.h>

#if !defined(__GCC_ATOMIC_INT_LOCK_FREE) || __GCC_ATOMIC_INT_LOCK_FREE != 2
#error "Process event capture requires lock-free integer atomics"
#endif
typedef char VcoEventSizeCheck[sizeof(VcoProcessEvent) == 32 ? 1 : -1];
typedef char VcoEventAtomicSizeCheck[sizeof(unsigned int) == sizeof(uint32_t) ? 1 : -1];

/* The SDK callbacks have no user context. These objects and this code must
 * remain resident even after unregister returns: SDK does not promise that
 * there are no in-flight callbacks. Never memset/reset this singleton. */
static uint32_t phase, invalidation, late_seen, gate;
/* Positive PID occupies 31 bits; high bit is sticky revocation. Callback
 * performs one CAS only. A mismatch means no matching watch or already dead.
 * Worker arm/disarm calls are serialized with each other, never callbacks. */
static uint32_t watched;
static int32_t native_result, registration_uid = -1;
static VcoProcessEvent queue[VCO_PROCESS_EVENTS_CAPACITY];
static unsigned head, count;

static uint32_t lose(void)
{
    return __atomic_fetch_or(&invalidation, VCO_PROCESS_EVENTS_LOST, __ATOMIC_ACQ_REL) |
           VCO_PROCESS_EVENTS_LOST;
}
static uint32_t invalidate(void)
{
    uint32_t current = __atomic_load_n(&invalidation, __ATOMIC_ACQUIRE);
    /* Bounded retry: callback never waits for another CPU. Saturation and
     * excessive contention permanently revoke usable observation history. */
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (current & VCO_PROCESS_EVENTS_LOST)
            return current;
        if (current == VCO_PROCESS_EVENTS_GENERATION_MASK)
            return lose();
        uint32_t next = current + 1;
        if (__atomic_compare_exchange_n(&invalidation, &current, next, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return next;
    }
    return lose();
}
static int try_gate(void)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&gate, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static void release_gate(void)
{
    __atomic_store_n(&gate, 0, __ATOMIC_RELEASE);
}
static int capture(uint32_t kind, SceUID pid, int event_id, int event_type, const void *parameter,
                   int parameter_has_pid)
{
    VcoProcessEvent event = {.invalidation = invalidate(),
                             .kind = kind,
                             .pid = pid,
                             .event_id = event_id,
                             .event_type = event_type,
                             .parameter_pid = -1};
    if ((kind == VCO_PROCESS_EVENT_EXIT || kind == VCO_PROCESS_EVENT_KILL) && pid > 0) {
        uint32_t expected = (uint32_t)pid;
        (void)__atomic_compare_exchange_n(&watched, &expected,
                                          (uint32_t)pid | VCO_PROCESS_EVENTS_LOST, 0,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    /* Invalidation above must precede metadata inspection and queue access. */
    uint32_t observed_phase = __atomic_load_n(&phase, __ATOMIC_ACQUIRE);
    if (observed_phase != VCO_PROCESS_EVENTS_ACTIVE) {
        event.flags |= VCO_PROCESS_EVENT_TRANSITION;
        if (observed_phase == VCO_PROCESS_EVENTS_STOPPING ||
            observed_phase == VCO_PROCESS_EVENTS_STOPPED ||
            observed_phase == VCO_PROCESS_EVENTS_UNREGISTER_FAILED) {
            event.flags |= VCO_PROCESS_EVENT_LATE;
            __atomic_store_n(&late_seen, 1, __ATOMIC_RELEASE);
        }
        event.invalidation = lose();
    }
    if (parameter) {
        event.flags |= VCO_PROCESS_EVENT_PARAM_PRESENT;
        if (parameter_has_pid) {
            const SceProcEventInvokeParam2 *p = parameter;
            event.parameter_size = p->size;
            if (p->size >= sizeof(*p)) {
                event.parameter_pid = p->pid;
                event.flags |= VCO_PROCESS_EVENT_PARAM_PID;
                if (kind == VCO_PROCESS_EVENT_SWITCH)
                    event.pid = p->pid;
            } else
                event.flags |= VCO_PROCESS_EVENT_PARAM_INVALID;
        } else {
            const SceProcEventInvokeParam1 *p = parameter;
            event.parameter_size = p->size;
            if (p->size < sizeof(*p))
                event.flags |= VCO_PROCESS_EVENT_PARAM_INVALID;
        }
    }
    if (event.pid <= 0)
        event.flags |= VCO_PROCESS_EVENT_PARAM_INVALID;
    if (event.flags & VCO_PROCESS_EVENT_PARAM_INVALID)
        event.invalidation = lose();
    if (!try_gate()) {
        lose();
        return 0;
    }
    if (count == VCO_PROCESS_EVENTS_CAPACITY)
        lose();
    else {
        queue[(head + count) % VCO_PROCESS_EVENTS_CAPACITY] = event;
        ++count;
    }
    release_gate();
    /* Observer only: do not veto/modify the system lifecycle operation. */
    return 0;
}
static int created(SceUID pid, SceProcEventInvokeParam2 *p, int unused)
{
    (void)unused;
    return capture(VCO_PROCESS_EVENT_CREATE, pid, 0, 0, p, 1);
}
static int exited(SceUID pid, SceProcEventInvokeParam1 *p, int unused)
{
    (void)unused;
    return capture(VCO_PROCESS_EVENT_EXIT, pid, 0, 0, p, 0);
}
static int killed(SceUID pid, SceProcEventInvokeParam1 *p, int unused)
{
    (void)unused;
    return capture(VCO_PROCESS_EVENT_KILL, pid, 0, 0, p, 0);
}
static int stopped(SceUID pid, int type, SceProcEventInvokeParam1 *p, int unused)
{
    (void)unused;
    return capture(VCO_PROCESS_EVENT_STOP, pid, 0, type, p, 0);
}
static int started(SceUID pid, int type, SceProcEventInvokeParam1 *p, int unused)
{
    (void)unused;
    return capture(VCO_PROCESS_EVENT_START, pid, 0, type, p, 0);
}
static int switched(int id, int type, SceProcEventInvokeParam2 *p, int unused)
{
    (void)unused;
    return capture(VCO_PROCESS_EVENT_SWITCH, -1, id, type, p, 1);
}
static const SceProcEventHandler handler = {.size = sizeof(SceProcEventHandler),
                                            .create = created,
                                            .exit = exited,
                                            .kill = killed,
                                            .stop = stopped,
                                            .start = started,
                                            .switch_process = switched};

int vco_process_events_start(void)
{
    uint32_t expected = VCO_PROCESS_EVENTS_NEW;
    if (!__atomic_compare_exchange_n(&phase, &expected, VCO_PROCESS_EVENTS_STARTING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -1;
    invalidate();
    SceUID result = ksceKernelRegisterProcEventHandler("VcoProcessEvidence", &handler, 0);
    __atomic_store_n(&native_result, result, __ATOMIC_RELAXED);
    if (result < 0) {
        lose();
        __atomic_store_n(&phase, VCO_PROCESS_EVENTS_REGISTER_FAILED, __ATOMIC_RELEASE);
        return result;
    }
    registration_uid = result;
    __atomic_store_n(&phase, VCO_PROCESS_EVENTS_ACTIVE, __ATOMIC_RELEASE);
    return 0;
}
int vco_process_events_stop(void)
{
    uint32_t expected = VCO_PROCESS_EVENTS_ACTIVE;
    if (!__atomic_compare_exchange_n(&phase, &expected, VCO_PROCESS_EVENTS_STOPPING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -1;
    /* Even a clean stop breaks continuity of lifecycle observation. */
    invalidate();
    lose();
    int result = ksceKernelUnregisterProcEventHandler(registration_uid);
    __atomic_store_n(&native_result, result, __ATOMIC_RELAXED);
    __atomic_store_n(&phase,
                     result < 0 ? VCO_PROCESS_EVENTS_UNREGISTER_FAILED : VCO_PROCESS_EVENTS_STOPPED,
                     __ATOMIC_RELEASE);
    return result < 0 ? result : 0;
}
VcoProcessEventsPoll vco_process_events_poll(VcoProcessEvent *out)
{
    if (!out)
        return VCO_PROCESS_EVENTS_ARGUMENT;
    if (!try_gate())
        return VCO_PROCESS_EVENTS_BUSY;
    VcoProcessEventsPoll result = VCO_PROCESS_EVENTS_EMPTY;
    if (count) {
        *out = queue[head];
        head = (head + 1) % VCO_PROCESS_EVENTS_CAPACITY;
        --count;
        result = VCO_PROCESS_EVENTS_OK;
    }
    release_gate();
    return result;
}
uint32_t vco_process_events_invalidation(void)
{
    return __atomic_load_n(&invalidation, __ATOMIC_ACQUIRE);
}
int vco_process_events_watch(int32_t pid, uint32_t epoch)
{
    if (pid <= 0 || (epoch & VCO_PROCESS_EVENTS_LOST) ||
        __atomic_load_n(&phase, __ATOMIC_ACQUIRE) != VCO_PROCESS_EVENTS_ACTIVE ||
        vco_process_events_invalidation() != epoch)
        return -1;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&watched, &expected, (uint32_t)pid, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return -1;
    if (!vco_process_events_allow(pid) || vco_process_events_invalidation() != epoch) {
        vco_process_events_unwatch(pid);
        return -1;
    }
    return 0;
}
void vco_process_events_unwatch(int32_t pid)
{
    uint32_t current = __atomic_load_n(&watched, __ATOMIC_ACQUIRE);
    if (pid > 0 && (current & VCO_PROCESS_EVENTS_GENERATION_MASK) == (uint32_t)pid)
        __atomic_store_n(&watched, 0, __ATOMIC_RELEASE);
}
int vco_process_events_allow(int32_t pid)
{
    return pid > 0 && __atomic_load_n(&phase, __ATOMIC_ACQUIRE) == VCO_PROCESS_EVENTS_ACTIVE &&
           __atomic_load_n(&watched, __ATOMIC_ACQUIRE) == (uint32_t)pid;
}
void vco_process_events_state(VcoProcessEventsState *out)
{
    if (!out)
        return;
    out->phase = __atomic_load_n(&phase, __ATOMIC_ACQUIRE);
    out->invalidation = vco_process_events_invalidation();
    out->late_callback_seen = __atomic_load_n(&late_seen, __ATOMIC_ACQUIRE);
    out->native_result = __atomic_load_n(&native_result, __ATOMIC_ACQUIRE);
}

#ifdef VCO_PROCESS_EVENTS_TESTING
/* Fault injection is absent from production objects. Tests use this only with
 * all callbacks/consumers quiescent; it is not a supported runtime interface. */
int vco_process_events_test_hold_queue(void)
{
    return try_gate();
}
void vco_process_events_test_release_queue(void)
{
    release_gate();
}
void vco_process_events_test_seed_generation(uint32_t generation)
{
    __atomic_store_n(&invalidation, generation, __ATOMIC_RELEASE);
}
#endif
