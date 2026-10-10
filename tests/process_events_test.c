#include "platform/vita/process_events.h"
#include "events_test_hooks.h"
#include <psp2kern/kernel/proc_event.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static const SceProcEventHandler *registered;
static int register_result = 77, unregister_result;
static unsigned register_calls, unregister_calls;
static unsigned hold_register, hold_unregister, register_entered, unregister_entered;
static unsigned register_callback;
static SceProcEventInvokeParam1 p1 = {.size = sizeof(p1)};
static SceProcEventInvokeParam2 p2 = {.size = sizeof(p2), .pid = 123};

SceUID ksceKernelRegisterProcEventHandler(const char *name, const SceProcEventHandler *handler,
                                          int a3)
{
    assert(!strcmp(name, "VcoProcessEvidence") && !a3);
    assert(handler && handler->size == sizeof(*handler));
    assert(handler->create && handler->exit && handler->kill && handler->stop && handler->start &&
           handler->switch_process);
    __atomic_add_fetch(&register_calls, 1, __ATOMIC_RELAXED);
    registered = handler;
    __atomic_store_n(&register_entered, 1, __ATOMIC_RELEASE);
    if (register_callback)
        assert(!handler->create(123, &p2, 42));
    while (__atomic_load_n(&hold_register, __ATOMIC_ACQUIRE))
        sched_yield();
    return register_result;
}
int ksceKernelUnregisterProcEventHandler(SceUID uid)
{
    assert(uid == 77);
    __atomic_add_fetch(&unregister_calls, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&unregister_entered, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&hold_unregister, __ATOMIC_ACQUIRE))
        sched_yield();
    return unregister_result;
}
static VcoProcessEventsState state(uint32_t phase)
{
    VcoProcessEventsState result;
    vco_process_events_state(&result);
    assert(result.phase == phase);
    assert(result.invalidation == vco_process_events_invalidation());
    return result;
}
static void begin(void)
{
    VcoProcessEventsState initial = state(VCO_PROCESS_EVENTS_NEW);
    assert(!initial.invalidation && !initial.late_callback_seen);
    assert(vco_process_events_stop() < 0 && !unregister_calls);
    assert(!vco_process_events_start());
    assert(register_calls == 1 && state(VCO_PROCESS_EVENTS_ACTIVE).native_result == 77);
    assert(vco_process_events_start() < 0 && register_calls == 1);
}
static VcoProcessEvent next(uint32_t kind)
{
    VcoProcessEvent event;
    assert(vco_process_events_poll(&event) == VCO_PROCESS_EVENTS_OK);
    assert(event.kind == kind);
    return event;
}
static void empty(void)
{
    VcoProcessEvent event, expected;
    memset(&event, 0xaa, sizeof(event));
    expected = event;
    assert(vco_process_events_poll(&event) == VCO_PROCESS_EVENTS_EMPTY);
    assert(!memcmp(&event, &expected, sizeof(event)));
}
static void callbacks(void)
{
    begin();
    assert(sizeof(VcoProcessEvent) == 32);
    assert(vco_process_events_poll(NULL) == VCO_PROCESS_EVENTS_ARGUMENT);
    vco_process_events_state(NULL);
    uint32_t before = vco_process_events_invalidation();
    assert(!registered->create(123, &p2, 987));
    assert(!registered->exit(123, &p1, 987));
    assert(!registered->kill(123, &p1, 987));
    assert(!registered->stop(123, 0x6543, &p1, 987));
    assert(!registered->start(123, -65, &p1, 987));
    assert(!registered->switch_process(0x11, 0x3210, &p2, 987));
    assert(vco_process_events_invalidation() == before + 6);
    for (unsigned i = 1; i <= 6; ++i) {
        VcoProcessEvent event = next(i);
        assert(event.pid == 123 && event.invalidation == before + i);
        assert(event.event_id == (i == VCO_PROCESS_EVENT_SWITCH ? 0x11 : 0));
        int type = i == VCO_PROCESS_EVENT_STOP ? 0x6543 : 0;
        if (i == VCO_PROCESS_EVENT_START)
            type = -65;
        if (i == VCO_PROCESS_EVENT_SWITCH)
            type = 0x3210;
        assert(event.event_type == type);
        int has_pid = i == VCO_PROCESS_EVENT_CREATE || i == VCO_PROCESS_EVENT_SWITCH;
        assert(event.parameter_size == (has_pid ? sizeof(p2) : sizeof(p1)));
        assert(event.parameter_pid == (has_pid ? 123 : -1));
        assert(event.flags ==
               (VCO_PROCESS_EVENT_PARAM_PRESENT | (has_pid ? VCO_PROCESS_EVENT_PARAM_PID : 0u)));
    }
    empty();
    /* Null optional metadata does not hide the explicit exit PID. */
    assert(!registered->exit(123, NULL, 0));
    VcoProcessEvent event = next(VCO_PROCESS_EVENT_EXIT);
    assert(event.pid == 123 && !event.flags && !event.parameter_size);
}
static void lifecycle(void)
{
    begin();
    assert(!vco_process_events_stop() && unregister_calls == 1);
    VcoProcessEventsState stopped = state(VCO_PROCESS_EVENTS_STOPPED);
    assert(stopped.invalidation & VCO_PROCESS_EVENTS_LOST);
    assert(!stopped.late_callback_seen);
    assert(vco_process_events_stop() < 0 && unregister_calls == 1);
    assert(vco_process_events_start() < 0 && register_calls == 1);
    /* Deliberately retain and invoke the SDK's callback pointer after success. */
    assert(!registered->exit(123, &p1, 0));
    VcoProcessEvent event = next(VCO_PROCESS_EVENT_EXIT);
    assert((event.flags & (VCO_PROCESS_EVENT_LATE | VCO_PROCESS_EVENT_TRANSITION)) ==
           (VCO_PROCESS_EVENT_LATE | VCO_PROCESS_EVENT_TRANSITION));
    assert(state(VCO_PROCESS_EVENTS_STOPPED).late_callback_seen);
}
static void register_failure(void)
{
    register_result = -88;
    assert(vco_process_events_start() == -88);
    VcoProcessEventsState s = state(VCO_PROCESS_EVENTS_REGISTER_FAILED);
    assert(s.native_result == -88 && (s.invalidation & VCO_PROCESS_EVENTS_LOST));
    assert(vco_process_events_start() < 0 && register_calls == 1);
    assert(vco_process_events_stop() < 0 && !unregister_calls);
}
static void unregister_failure(void)
{
    begin();
    unregister_result = -99;
    assert(vco_process_events_stop() == -99);
    VcoProcessEventsState s = state(VCO_PROCESS_EVENTS_UNREGISTER_FAILED);
    assert(s.native_result == -99 && (s.invalidation & VCO_PROCESS_EVENTS_LOST));
    assert(vco_process_events_stop() < 0 && unregister_calls == 1);
    assert(vco_process_events_start() < 0 && register_calls == 1);
    assert(!registered->create(123, &p2, 0));
    assert(next(VCO_PROCESS_EVENT_CREATE).flags & VCO_PROCESS_EVENT_LATE);
}
static void overflow(void)
{
    begin();
    for (unsigned i = 0; i < VCO_PROCESS_EVENTS_CAPACITY + 10; ++i)
        assert(!registered->kill((int)i + 1, NULL, 0));
    assert(vco_process_events_invalidation() & VCO_PROCESS_EVENTS_LOST);
    for (unsigned i = 0; i < VCO_PROCESS_EVENTS_CAPACITY; ++i)
        assert(next(VCO_PROCESS_EVENT_KILL).pid == (int)i + 1);
    empty();
    assert(!registered->exit(123, &p1, 0));
    assert(next(VCO_PROCESS_EVENT_EXIT).invalidation & VCO_PROCESS_EVENTS_LOST);
    assert(vco_process_events_invalidation() & VCO_PROCESS_EVENTS_LOST);
}
static void contention(void)
{
    begin();
    assert(vco_process_events_test_hold_queue());
    VcoProcessEvent event, expected;
    memset(&event, 0xbb, sizeof(event));
    expected = event;
    assert(vco_process_events_poll(&event) == VCO_PROCESS_EVENTS_BUSY);
    assert(!memcmp(&event, &expected, sizeof(event)));
    uint32_t before = vco_process_events_invalidation();
    assert(!registered->exit(123, &p1, 0));
    uint32_t after = vco_process_events_invalidation();
    assert((after & VCO_PROCESS_EVENTS_LOST) &&
           (after & VCO_PROCESS_EVENTS_GENERATION_MASK) == before + 1);
    vco_process_events_test_release_queue();
    empty();
    assert(vco_process_events_invalidation() & VCO_PROCESS_EVENTS_LOST);
}
static void exhaustion(void)
{
    begin();
    vco_process_events_test_seed_generation(VCO_PROCESS_EVENTS_GENERATION_MASK - 1);
    assert(!registered->exit(123, &p1, 0));
    assert(vco_process_events_invalidation() == VCO_PROCESS_EVENTS_GENERATION_MASK);
    assert(!registered->exit(123, &p1, 0));
    assert(vco_process_events_invalidation() == UINT32_MAX);
    for (unsigned i = 0; i < 100; ++i)
        assert(!registered->exit(123, &p1, 0));
    assert(vco_process_events_invalidation() == UINT32_MAX);
}
static void invalid_metadata(void)
{
    begin();
    /* The size field is all that is accessible: ASan catches a speculative
     * read of the omitted PID/unknown fields in a short parameter block. */
    SceSize short_parameter = sizeof(SceSize);
    assert(
        !registered->switch_process(0x1234, -4, (SceProcEventInvokeParam2 *)&short_parameter, 0));
    VcoProcessEvent event = next(VCO_PROCESS_EVENT_SWITCH);
    assert(event.pid == -1 && event.parameter_pid == -1);
    assert(event.parameter_size == sizeof(short_parameter));
    assert(event.flags & VCO_PROCESS_EVENT_PARAM_INVALID);
    assert(event.invalidation & VCO_PROCESS_EVENTS_LOST);
    assert(!registered->switch_process(17, 2, NULL, 0));
    event = next(VCO_PROCESS_EVENT_SWITCH);
    assert(event.pid == -1 && (event.flags & VCO_PROCESS_EVENT_PARAM_INVALID));
    assert(!registered->exit(-1, &p1, 0));
    assert(next(VCO_PROCESS_EVENT_EXIT).flags & VCO_PROCESS_EVENT_PARAM_INVALID);
    assert(!registered->kill(123, (SceProcEventInvokeParam1 *)&short_parameter, 0));
    assert(next(VCO_PROCESS_EVENT_KILL).flags & VCO_PROCESS_EVENT_PARAM_INVALID);
}
static void transition_callback(void)
{
    register_callback = 1;
    assert(!vco_process_events_start());
    VcoProcessEvent event = next(VCO_PROCESS_EVENT_CREATE);
    assert(event.flags & VCO_PROCESS_EVENT_TRANSITION);
    assert(!(event.flags & VCO_PROCESS_EVENT_LATE));
    assert(event.invalidation & VCO_PROCESS_EVENTS_LOST);
    assert(state(VCO_PROCESS_EVENTS_ACTIVE).invalidation & VCO_PROCESS_EVENTS_LOST);
}
static void *start_thread(void *unused)
{
    (void)unused;
    assert(!vco_process_events_start());
    return NULL;
}
static void *stop_thread(void *unused)
{
    (void)unused;
    assert(!vco_process_events_stop());
    return NULL;
}
static void lifecycle_races(void)
{
    pthread_t thread;
    hold_register = 1;
    assert(!pthread_create(&thread, NULL, start_thread, NULL));
    while (!__atomic_load_n(&register_entered, __ATOMIC_ACQUIRE))
        sched_yield();
    assert(state(VCO_PROCESS_EVENTS_STARTING).phase == VCO_PROCESS_EVENTS_STARTING);
    assert(vco_process_events_start() < 0 && vco_process_events_stop() < 0);
    __atomic_store_n(&hold_register, 0, __ATOMIC_RELEASE);
    assert(!pthread_join(thread, NULL));
    assert(register_calls == 1);
    hold_unregister = 1;
    assert(!pthread_create(&thread, NULL, stop_thread, NULL));
    while (!__atomic_load_n(&unregister_entered, __ATOMIC_ACQUIRE))
        sched_yield();
    assert(state(VCO_PROCESS_EVENTS_STOPPING).phase == VCO_PROCESS_EVENTS_STOPPING);
    assert(vco_process_events_start() < 0 && vco_process_events_stop() < 0);
    assert(!registered->exit(123, &p1, 0));
    assert(next(VCO_PROCESS_EVENT_EXIT).flags & VCO_PROCESS_EVENT_LATE);
    __atomic_store_n(&hold_unregister, 0, __ATOMIC_RELEASE);
    assert(!pthread_join(thread, NULL));
    assert(unregister_calls == 1 && state(VCO_PROCESS_EVENTS_STOPPED).late_callback_seen);
}

static unsigned go, producers_done, consumed;
static void *producer(void *arg)
{
    unsigned producer_id = *(unsigned *)arg;
    SceProcEventInvokeParam2 parameter = {.size = sizeof(parameter), .pid = (int)producer_id + 100};
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE))
        sched_yield();
    for (unsigned i = 0; i < 2000; ++i)
        assert(!registered->create(parameter.pid, &parameter, 0));
    __atomic_add_fetch(&producers_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void *consumer(void *unused)
{
    (void)unused;
    for (;;) {
        VcoProcessEvent event;
        VcoProcessEventsPoll result = vco_process_events_poll(&event);
        if (result == VCO_PROCESS_EVENTS_OK) {
            assert(event.kind == VCO_PROCESS_EVENT_CREATE && event.pid >= 100 && event.pid < 104);
            assert(event.parameter_pid == event.pid && event.parameter_size == sizeof(p2));
            assert(!(event.flags & VCO_PROCESS_EVENT_PARAM_INVALID));
            ++consumed;
        } else if (result == VCO_PROCESS_EVENTS_EMPTY &&
                   __atomic_load_n(&producers_done, __ATOMIC_ACQUIRE) == 4)
            break;
        else {
            assert(result == VCO_PROCESS_EVENTS_BUSY || result == VCO_PROCESS_EVENTS_EMPTY);
            sched_yield();
        }
    }
    return NULL;
}
static void concurrent(void)
{
    begin();
    pthread_t threads[5];
    unsigned ids[4] = {0, 1, 2, 3};
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&threads[i], NULL, producer, &ids[i]));
    assert(!pthread_create(&threads[4], NULL, consumer, NULL));
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 5; ++i)
        assert(!pthread_join(threads[i], NULL));
    assert(consumed <= 8000 && producers_done == 4);
    /* If no history was lost, every completed callback must be present. */
    if (!(vco_process_events_invalidation() & VCO_PROCESS_EVENTS_LOST))
        assert(consumed == 8000);
    empty();
    assert(!vco_process_events_stop());
}
static void terminal_watch(void)
{
    assert(vco_process_events_watch(123, 0) < 0);
    assert(!vco_process_events_allow(123));
    begin();
    uint32_t epoch = vco_process_events_invalidation();
    assert(vco_process_events_watch(0, epoch) < 0);
    assert(vco_process_events_watch(123, epoch - 1) < 0);
    assert(!vco_process_events_watch(123, epoch));
    assert(vco_process_events_watch(124, epoch) < 0);
    assert(vco_process_events_allow(123));
    vco_process_events_unwatch(124);
    assert(vco_process_events_allow(123));
    assert(!registered->exit(124, &p1, 0));
    assert(!registered->stop(123, 0, &p1, 0));
    assert(vco_process_events_allow(123));
    assert(!registered->exit(123, &p1, 0));
    assert(!vco_process_events_allow(123)); /* Before dequeue. */
    while (vco_process_events_poll(&(VcoProcessEvent){0}) == VCO_PROCESS_EVENTS_OK) {
    }
    assert(!vco_process_events_allow(123)); /* Sticky after dequeue. */
    vco_process_events_unwatch(123);
    assert(!vco_process_events_watch(124, vco_process_events_invalidation()));
    assert(!registered->exit(123, &p1, 0));
    assert(vco_process_events_allow(124));
    assert(vco_process_events_test_hold_queue());
    assert(!registered->kill(124, &p1, 0));
    assert(!vco_process_events_allow(124)); /* Queue contention cannot hide it. */
    vco_process_events_test_release_queue();
    vco_process_events_unwatch(124);
    assert(vco_process_events_watch(125, vco_process_events_invalidation()) < 0);
    assert(!vco_process_events_stop());
    assert(!vco_process_events_allow(125));
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "terminal_watch"))
        terminal_watch();
    else if (!strcmp(argv[1], "callbacks"))
        callbacks();
    else if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "register_failure"))
        register_failure();
    else if (!strcmp(argv[1], "unregister_failure"))
        unregister_failure();
    else if (!strcmp(argv[1], "overflow"))
        overflow();
    else if (!strcmp(argv[1], "contention"))
        contention();
    else if (!strcmp(argv[1], "exhaustion"))
        exhaustion();
    else if (!strcmp(argv[1], "invalid_metadata"))
        invalid_metadata();
    else if (!strcmp(argv[1], "transition_callback"))
        transition_callback();
    else if (!strcmp(argv[1], "lifecycle_races"))
        lifecycle_races();
    else if (!strcmp(argv[1], "concurrent"))
        concurrent();
    else
        return 1;
    puts("process event tests passed");
    return 0;
}
