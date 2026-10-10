#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <psp2kern/kernel/threadmgr.h>
#include "platform/vita/control_worker.h"

static VcoVitaControlWorker worker;
static VcoControlMailbox mailbox;
static VcoEngine engine;
static struct {
    SceKernelThreadEntry entry;
    uint8_t argument[sizeof(void *)];
    SceSize length;
    int create_error, start_error, wait_error, delete_error, delay_error, exit_status;
    unsigned creates, starts, waits, deletes, delays, updates;
    uint32_t timeout;
    int64_t now;
    bool live, started, exited, immediate;
    void (*on_delay)(void);
} sdk;
static void run_thread(void);

SceUID ksceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority,
                              SceSize stack, SceUInt attr, int affinity,
                              const SceKernelThreadOptParam *option)
{
    assert(!strcmp(name, "vco_execution") && entry && priority == 0x10000100);
    assert(stack >= 8192 && !attr && !affinity && !option && !sdk.live);
    ++sdk.creates;
    if (sdk.create_error)
        return sdk.create_error;
    sdk.entry = entry;
    sdk.live = true;
    sdk.started = sdk.exited = false;
    return 123;
}
int ksceKernelStartThread(SceUID thread, SceSize length, void *argument)
{
    assert(thread == 123 && sdk.live && !sdk.started && length == sizeof(void *) && argument);
    ++sdk.starts;
    if (sdk.start_error)
        return sdk.start_error;
    memcpy(sdk.argument, argument, length);
    sdk.length = length;
    sdk.started = true;
    if (sdk.immediate)
        run_thread();
    return 0;
}
static void run_thread(void)
{
    assert(sdk.live && sdk.started && !sdk.exited);
    sdk.exit_status = sdk.entry(sdk.length, sdk.argument);
    sdk.exited = true;
}
int ksceKernelWaitThreadEnd(SceUID thread, int *status, SceUInt *timeout)
{
    assert(thread == 123 && sdk.live && sdk.started && status && timeout);
    ++sdk.waits;
    sdk.timeout = *timeout;
    if (sdk.wait_error)
        return sdk.wait_error;
    if (!sdk.exited)
        run_thread();
    *status = sdk.exit_status;
    return 0;
}
int ksceKernelDeleteThread(SceUID thread)
{
    assert(thread == 123 && sdk.live && (!sdk.started || sdk.exited));
    ++sdk.deletes;
    if (sdk.delete_error)
        return sdk.delete_error;
    sdk.live = false;
    return 0;
}
int ksceKernelDelayThread(SceUInt delay)
{
    assert(delay == VCO_VITA_WORKER_PERIOD_US);
    ++sdk.delays;
    assert(sdk.delays < 10); /* A broken stop path must not hang the test. */
    sdk.now += delay;
    if (sdk.on_delay)
        sdk.on_delay();
    return sdk.delay_error;
}
SceInt64 ksceKernelGetSystemTimeWide(void)
{
    return sdk.now;
}

static void setup(void)
{
    assert(!sdk.live);
    memset(&sdk, 0, sizeof(sdk));
    sdk.now = 100000;
    vco_engine_init(&engine);
    vco_mailbox_init(&mailbox, &engine, 88);
    vco_vita_control_worker_init(&worker);
}
static void update(void *context, uint64_t now)
{
    assert(context == &sdk && now == (uint64_t)sdk.now);
    ++sdk.updates;
}
static VcoControlRequest request(void)
{
    return (VcoControlRequest){
        1, 1, 1, VCO_CONTROL_ABI, sizeof(VcoControlRequest), VCO_CONTROL_QUERY, 0, 0, 1, {0, 0}};
}
static void lifecycle(void)
{
    setup();
    VcoError cleanup = VCO_E_IO;
    assert(vco_vita_control_worker_start(NULL, &mailbox, NULL, NULL) == VCO_VITA_WORKER_ARGUMENT);
    assert(vco_vita_control_worker_start(&worker, NULL, NULL, NULL) == VCO_VITA_WORKER_ARGUMENT);
    assert(vco_vita_control_worker_stop(NULL, 1000, &cleanup) == VCO_VITA_WORKER_ARGUMENT);
    assert(!vco_vita_control_worker_stop(&worker, 1000, &cleanup) && cleanup == VCO_OK);
    assert(!sdk.creates && !sdk.deletes);
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    assert(vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL) == VCO_VITA_WORKER_STATE);
    assert(!vco_vita_control_worker_stop(&worker, 1234, &cleanup) && cleanup == VCO_OK);
    assert(sdk.timeout == 1234 && sdk.waits == 1 && sdk.deletes == 1 && !sdk.updates);
    assert(worker.thread < 0 && worker.completed && vco_mailbox_stop_result(&mailbox, &cleanup));
    assert(!vco_vita_control_worker_stop(&worker, 1234, NULL));
    assert(sdk.waits == 1 && sdk.deletes == 1);
    assert(vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL) == VCO_VITA_WORKER_STATE);
    VcoControlRequest packet = request();
    VcoControlReply reply;
    uint64_t ticket = 0;
    assert(vco_vita_control_worker_submit(&worker, 88, &packet, sizeof(packet), &ticket) ==
           VCO_MAILBOX_CLOSED);
    assert(vco_vita_control_worker_receive(&worker, 88, 1, &reply) == VCO_MAILBOX_CLOSED);
    assert(vco_vita_control_worker_ack(&worker, 88, 1) == VCO_MAILBOX_CLOSED);
    assert(vco_vita_control_worker_resume(&worker, 88, &packet, &ticket) == VCO_MAILBOX_CLOSED);
    assert(vco_vita_control_worker_submit(NULL, 88, &packet, sizeof(packet), &ticket) ==
           VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_control_worker_receive(NULL, 88, 1, &reply) == VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_control_worker_ack(NULL, 88, 1) == VCO_MAILBOX_ARGUMENT);
    assert(vco_vita_control_worker_resume(NULL, 88, &packet, &ticket) == VCO_MAILBOX_ARGUMENT);
    VcoControlMailbox closing, invalid;
    vco_mailbox_init(&closing, &engine, 88);
    vco_mailbox_stop(&closing);
    assert(vco_vita_control_worker_start(&worker, &closing, NULL, NULL) == VCO_VITA_WORKER_STATE);
    vco_mailbox_init(&invalid, NULL, 88);
    assert(vco_vita_control_worker_start(&worker, &invalid, NULL, NULL) == VCO_VITA_WORKER_STATE);
    vco_mailbox_init(&invalid, &engine, -1);
    assert(vco_vita_control_worker_start(&worker, &invalid, NULL, NULL) == VCO_VITA_WORKER_STATE);
}
static void failures(void)
{
    VcoError cleanup;
    setup();
    sdk.create_error = -101;
    assert(vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL) == -101);
    assert(worker.thread < 0 && !sdk.live && !sdk.starts && !sdk.deletes);
    assert(!vco_vita_control_worker_stop(&worker, 0, NULL));
    assert(!vco_mailbox_stop_result(&mailbox, &cleanup));
    sdk.create_error = 0;
    sdk.start_error = -102;
    assert(vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL) == -102);
    assert(worker.thread < 0 && !sdk.live && sdk.deletes == 1 && !sdk.waits);
    assert(!vco_mailbox_stop_result(&mailbox, &cleanup));
    sdk.delete_error = -103;
    assert(vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL) == -102);
    assert(worker.thread == 123 && sdk.live && !worker.started);
    assert(vco_vita_control_worker_stop(&worker, 1000, NULL) == -103 && !sdk.waits);
    sdk.delete_error = 0;
    assert(!vco_vita_control_worker_stop(&worker, 1000, NULL));
    assert(!sdk.waits && !sdk.live && !vco_mailbox_stop_result(&mailbox, &cleanup));
    sdk.start_error = 0;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    sdk.wait_error = -104;
    unsigned deletes = sdk.deletes;
    assert(vco_vita_control_worker_stop(&worker, 17, NULL) == -104);
    assert(worker.thread == 123 && sdk.live && !worker.joined && sdk.deletes == deletes);
    assert(sdk.timeout == 17 && !sdk.updates);
    sdk.wait_error = 0;
    sdk.delete_error = -105;
    assert(vco_vita_control_worker_stop(&worker, 200, &cleanup) == -105 && cleanup == VCO_OK);
    assert(worker.thread == 123 && sdk.exited && worker.joined);
    unsigned waits = sdk.waits;
    sdk.delete_error = 0;
    assert(!vco_vita_control_worker_stop(&worker, 0, &cleanup));
    assert(sdk.waits == waits && !sdk.live);

    setup();
    engine.blocked_restore = true;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL));
    assert(vco_vita_control_worker_stop(&worker, 1000, &cleanup) == VCO_VITA_WORKER_CLEANUP);
    assert(cleanup == VCO_E_RESTORE && !sdk.live && engine.blocked_restore);
    assert(vco_vita_control_worker_stop(&worker, 0, &cleanup) == VCO_VITA_WORKER_CLEANUP);
    VcoControlMailbox fresh;
    vco_mailbox_init(&fresh, &engine, 88);
    assert(vco_vita_control_worker_start(&worker, &fresh, NULL, NULL) == VCO_VITA_WORKER_STATE);

    setup();
    assert(!vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL));
    /* Simulate an unexpected early thread return without mailbox cleanup. */
    sdk.exited = true;
    sdk.exit_status = 0;
    assert(vco_vita_control_worker_stop(&worker, 1000, &cleanup) == VCO_VITA_WORKER_STATE);
    assert(!sdk.live && worker.joined && !worker.completed);
    assert(!vco_mailbox_stop_result(&mailbox, &cleanup));
    vco_mailbox_init(&fresh, &engine, 88);
    assert(vco_vita_control_worker_start(&worker, &fresh, NULL, NULL) == VCO_VITA_WORKER_STATE);
    assert(vco_vita_control_worker_stop(&worker, 0, NULL) == VCO_VITA_WORKER_STATE);

    setup();
    sdk.delay_error = -106;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    run_thread();
    assert(sdk.updates == 1 && sdk.delays == 1 && !worker.accepting);
    assert(vco_vita_control_worker_stop(&worker, 1000, &cleanup) == -106 && cleanup == VCO_OK);
    assert(!sdk.live && vco_mailbox_stop_result(&mailbox, &cleanup));

    setup();
    sdk.now = -1;
    sdk.immediate = true;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    assert(!sdk.updates && !sdk.delays && !worker.accepting);
    assert(vco_vita_control_worker_stop(&worker, 1000, &cleanup) == VCO_VITA_WORKER_CLOCK);
    assert(!sdk.live && cleanup == VCO_OK);
}

static uint64_t transport_ticket;
static void inspect_reply(void)
{
    VcoControlReply reply, again;
    VcoControlRequest recovered;
    uint64_t recovered_ticket = 0;
    assert(sdk.updates == 1);
    assert(!vco_vita_control_worker_resume(&worker, 88, &recovered, &recovered_ticket));
    assert(recovered.request == 1 && recovered_ticket == transport_ticket);
    assert(vco_vita_control_worker_receive(&worker, 77, transport_ticket, &reply) ==
           VCO_MAILBOX_OWNER);
    assert(!vco_vita_control_worker_receive(&worker, 88, transport_ticket, &reply));
    assert(reply.request == 1 && reply.status == VCO_CONTROL_UNBOUND);
    assert(!vco_vita_control_worker_receive(&worker, 88, transport_ticket, &again));
    assert(!memcmp(&reply, &again, sizeof(reply)));
    assert(!vco_vita_control_worker_ack(&worker, 88, transport_ticket));
    assert(vco_vita_control_worker_receive(&worker, 88, transport_ticket, &reply) ==
           VCO_MAILBOX_TICKET);
    assert(vco_vita_control_worker_resume(&worker, 88, &recovered, &recovered_ticket) ==
           VCO_MAILBOX_EMPTY);
    /* A delay failure exercises worker-initiated stop, after transport finishes. */
    sdk.delay_error = -107;
}
static void transport(void)
{
    setup();
    VcoControlRequest packet = request();
    VcoControlReply reply;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    VcoControlRequest recovered;
    uint64_t recovered_ticket = 0;
    assert(vco_vita_control_worker_resume(&worker, 88, &recovered, &recovered_ticket) ==
           VCO_MAILBOX_EMPTY);
    assert(vco_vita_control_worker_submit(&worker, 77, &packet, sizeof(packet),
                                          &transport_ticket) == VCO_MAILBOX_OWNER);
    assert(
        !vco_vita_control_worker_submit(&worker, 88, &packet, sizeof(packet), &transport_ticket));
    assert(transport_ticket != 0);
    assert(vco_vita_control_worker_receive(&worker, 88, transport_ticket, &reply) ==
           VCO_MAILBOX_WAIT);
    uint64_t duplicate = 0;
    assert(!vco_vita_control_worker_submit(&worker, 88, &packet, sizeof(packet), &duplicate));
    assert(duplicate == transport_ticket);
    assert(vco_vita_control_worker_resume(&worker, 77, &recovered, &recovered_ticket) ==
           VCO_MAILBOX_OWNER);
    assert(!vco_vita_control_worker_resume(&worker, 88, &recovered, &recovered_ticket));
    assert(!memcmp(&packet, &recovered, sizeof(packet)) && recovered_ticket == transport_ticket);
    packet.request = 999;
    assert(vco_vita_control_worker_submit(&worker, 88, &packet, sizeof(packet), &duplicate) ==
           VCO_MAILBOX_BUSY);
    sdk.on_delay = inspect_reply;
    run_thread();
    assert(sdk.exited && sdk.delays == 1);
    assert(vco_vita_control_worker_stop(&worker, 1000, NULL) == -107 && !sdk.live);
}
static void reap_stopped(void)
{
    setup();
    VcoError cleanup = VCO_OK;
    assert(vco_vita_control_worker_reap_stopped(&worker, &cleanup) == VCO_VITA_WORKER_STATE);
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    assert(vco_vita_control_worker_reap_stopped(&worker, &cleanup) == VCO_VITA_WORKER_STATE);
    assert(!worker.stop_requested && !sdk.waits && !sdk.deletes);
    worker.accepting = 0; /* Failure has closed admission, thread still in flight. */
    sdk.wait_error = -110;
    assert(vco_vita_control_worker_reap_stopped(&worker, &cleanup) == -110);
    assert(!sdk.timeout && !worker.joined && !sdk.deletes && sdk.live);
    sdk.wait_error = 0;
    sdk.delete_error = -111;
    assert(vco_vita_control_worker_reap_stopped(&worker, &cleanup) == -111);
    assert(worker.joined && worker.completed && worker.thread == 123 && sdk.live);
    unsigned waits = sdk.waits;
    sdk.delete_error = 0;
    assert(!vco_vita_control_worker_reap_stopped(&worker, &cleanup) && cleanup == VCO_OK);
    assert(!sdk.live && sdk.waits == waits && !sdk.updates);
    assert(!vco_vita_control_worker_reap_stopped(&worker, &cleanup) && sdk.waits == waits);

    setup();
    sdk.immediate = true;
    sdk.delay_error = -112;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    assert(sdk.exited && !worker.accepting && sdk.updates == 1);
    assert(!vco_vita_control_worker_reap_stopped(&worker, &cleanup));
    assert(worker.run_error == -112 && cleanup == VCO_OK && !sdk.live);

    setup();
    engine.blocked_restore = true;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL));
    worker.accepting = 0;
    assert(!vco_vita_control_worker_reap_stopped(&worker, &cleanup));
    assert(cleanup == VCO_E_RESTORE && engine.blocked_restore && worker.completed && !sdk.live);

    setup();
    assert(!vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL));
    sdk.exited = true; /* An exited thread alone does not prove mailbox cleanup. */
    worker.accepting = 0;
    assert(vco_vita_control_worker_reap_stopped(&worker, &cleanup) == VCO_VITA_WORKER_STATE);
    assert(worker.joined && !worker.completed && !sdk.live);
    assert(vco_vita_control_worker_reap_stopped(&worker, &cleanup) == VCO_VITA_WORKER_STATE);
}
static unsigned finalize_calls;
static VcoError finalize_result;
static VcoError finalize(void *context)
{
    VcoError result;
    assert(context == &sdk && !worker.accepting && !sdk.exited);
    assert(vco_mailbox_stop_result(&mailbox, &result));
    ++finalize_calls;
    return finalize_result;
}
static void finalization(void)
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        setup();
        finalize_calls = 0;
        finalize_result = mode == 3 ? VCO_E_RESTORE : VCO_OK;
        worker.finalize = finalize;
        if (mode == 1)
            sdk.now = -1;
        if (mode == 2)
            sdk.delay_error = -99;
        assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
        if (mode == 1 || mode == 2)
            run_thread();
        VcoError cleanup = VCO_OK;
        int result = vco_vita_control_worker_stop(&worker, 1000, &cleanup);
        assert(finalize_calls == 1 && cleanup == finalize_result && !sdk.live);
        assert(mode == 0 ? result == 0 : result < 0);
        (void)vco_vita_control_worker_stop(&worker, 1000, &cleanup);
        assert(finalize_calls == 1);
        if (mode == 3) {
            vco_mailbox_init(&mailbox, &engine, 88);
            assert(vco_vita_control_worker_start(&worker, &mailbox, update, &sdk) ==
                   VCO_VITA_WORKER_STATE);
        }
    }
    setup();
    finalize_calls = 0;
    worker.finalize = finalize;
    sdk.start_error = -98;
    assert(vco_vita_control_worker_start(&worker, &mailbox, update, &sdk) == -98);
    (void)vco_vita_control_worker_stop(&worker, 1000, NULL);
    assert(!finalize_calls);
}

static VcoVitaControlShutdown sentinel(void)
{
    return (VcoVitaControlShutdown){
        .run_error = -8765, .cleanup = VCO_E_IO, .mailbox_complete = true};
}
static void assert_pending(void)
{
    VcoVitaControlShutdown out = sentinel();
    assert(!vco_vita_control_worker_shutdown(&worker, &out));
    assert(out.run_error == -8765 && out.cleanup == VCO_E_IO && out.mailbox_complete);
}
static void async_stop(void)
{
    setup();
    assert(vco_vita_control_worker_request_stop(NULL) == VCO_VITA_WORKER_ARGUMENT);
    assert(vco_vita_control_worker_shutdown(NULL, NULL) == VCO_VITA_WORKER_ARGUMENT);
    VcoVitaControlShutdown out = sentinel();
    assert(vco_vita_control_worker_shutdown(NULL, &out) == VCO_VITA_WORKER_ARGUMENT);
    assert(vco_vita_control_worker_shutdown(&worker, NULL) == VCO_VITA_WORKER_ARGUMENT);
    assert_pending();
    assert(vco_vita_control_worker_request_stop(&worker) == VCO_VITA_WORKER_STATE);
    assert(!worker.stop_requested && !sdk.waits && !sdk.deletes && !sdk.creates);
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    assert_pending();
    assert(!vco_vita_control_worker_request_stop(&worker));
    assert(!vco_vita_control_worker_request_stop(&worker));
    assert(worker.stop_requested && !worker.accepting && !sdk.waits && !sdk.deletes);
    assert_pending(); /* Closing transport admission is not a cleanup ACK. */
    VcoControlRequest packet = request();
    uint64_t ticket = 0;
    assert(vco_vita_control_worker_submit(&worker, 88, &packet, sizeof(packet), &ticket) ==
           VCO_MAILBOX_CLOSED);
    run_thread();
    assert(vco_vita_control_worker_shutdown(&worker, &out) == 1);
    assert(out.run_error == 0 && out.cleanup == VCO_OK && out.mailbox_complete);
    assert(worker.thread == 123 && !worker.joined && sdk.live);
    assert(!sdk.waits && !sdk.deletes && !sdk.updates);
    assert(!vco_vita_control_worker_request_stop(&worker));
    assert(!vco_vita_control_worker_stop(&worker, 1000, NULL));
    assert(worker.completed && sdk.waits == 1 && sdk.deletes == 1);
    VcoVitaControlShutdown again;
    assert(vco_vita_control_worker_shutdown(&worker, &again) == 1);
    assert(again.run_error == out.run_error && again.cleanup == out.cleanup &&
           again.mailbox_complete == out.mailbox_complete);

    setup();
    sdk.start_error = -99;
    assert(vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL) == -99);
    assert_pending(); /* Failed start must not acknowledge caller-owned state. */
    assert(vco_vita_control_worker_request_stop(&worker) == VCO_VITA_WORKER_STATE);
}
static VcoError ack_finalize(void *context)
{
    assert(context == &sdk && !worker.accepting);
    assert_pending(); /* Engine cleanup may be done, native receipts are not. */
    VcoError cleanup;
    assert(vco_mailbox_stop_result(&mailbox, &cleanup));
    ++finalize_calls;
    return finalize_result;
}
static void shutdown_failures(void)
{
    for (unsigned mode = 0; mode < 6; ++mode) {
        setup();
        worker.finalize = ack_finalize;
        finalize_calls = 0;
        finalize_result = mode == 1 || mode == 5 ? VCO_E_RESTORE : VCO_OK;
        if (mode == 2 || mode == 5)
            sdk.now = -1;
        if (mode == 3)
            sdk.delay_error = -98;
        if (mode == 4)
            engine.blocked_restore = true;
        assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
        if (mode != 2 && mode != 3 && mode != 5)
            assert(!vco_vita_control_worker_request_stop(&worker));
        run_thread();
        VcoVitaControlShutdown out;
        assert(vco_vita_control_worker_shutdown(&worker, &out) == 1);
        assert(out.mailbox_complete && finalize_calls == 1 && !sdk.waits && !sdk.deletes);
        assert(out.run_error == (mode == 2 || mode == 5 ? VCO_VITA_WORKER_CLOCK
                                 : mode == 3            ? -98
                                                        : 0));
        assert(out.cleanup == (mode == 1 || mode == 4 || mode == 5 ? VCO_E_RESTORE : VCO_OK));
        VcoError cleanup = VCO_OK;
        (void)vco_vita_control_worker_stop(&worker, 1000, &cleanup);
        assert(cleanup == out.cleanup && finalize_calls == 1 && worker.completed && !sdk.live);
    }
    setup();
    assert(!vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL));
    /* Native thread termination without the owning function's ACK cannot
     * establish cleanup. Neither polling nor join fabricates success. */
    sdk.exited = true;
    sdk.exit_status = 0;
    assert_pending();
    assert(vco_vita_control_worker_stop(&worker, 1000, NULL) == VCO_VITA_WORKER_STATE);
    assert_pending();
    assert(!worker.completed && !sdk.live);
}

static void shutdown_restart(void)
{
    setup();
    assert(!vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL));
    assert(!vco_vita_control_worker_stop(&worker, 1000, NULL));
    VcoVitaControlShutdown out;
    assert(vco_vita_control_worker_shutdown(&worker, &out) == 1);
    vco_mailbox_init(&mailbox, &engine, 88);
    sdk.create_error = -97;
    assert(vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL) == -97);
    assert_pending(); /* Previous lifetime's ACK is not the new context's ACK. */
    assert(vco_vita_control_worker_request_stop(&worker) == VCO_VITA_WORKER_STATE);
    sdk.create_error = 0;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, NULL, NULL));
    assert_pending();
    assert(!vco_vita_control_worker_stop(&worker, 1000, NULL));
    assert(vco_vita_control_worker_shutdown(&worker, &out) == 1 && out.cleanup == VCO_OK);

    setup();
    worker.finalize = ack_finalize;
    finalize_calls = 0;
    finalize_result = VCO_E_RESTORE;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    assert(!vco_vita_control_worker_request_stop(&worker));
    run_thread();
    sdk.exit_status = -96; /* A inconsistent native exit must not erase RESTORE. */
    VcoError cleanup = VCO_OK;
    assert(vco_vita_control_worker_stop(&worker, 1000, &cleanup) == VCO_VITA_WORKER_STATE);
    assert(cleanup == VCO_E_RESTORE && !worker.completed && !sdk.live);
    assert(vco_vita_control_worker_shutdown(&worker, &out) == 1);
    assert(out.run_error == 0 && out.cleanup == VCO_E_RESTORE && finalize_calls == 1);
}

static pthread_mutex_t phase_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t phase_cond = PTHREAD_COND_INITIALIZER;
static unsigned test_phase;
static bool release_operation, release_finalize;
static unsigned finalized_payload;

static void blocking_update(void *context, uint64_t now)
{
    assert(context == &sdk && now == (uint64_t)sdk.now);
    assert(!pthread_mutex_lock(&phase_mutex));
    test_phase = 1;
    assert(!pthread_cond_broadcast(&phase_cond));
    while (!release_operation)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    ++sdk.updates;
}
static VcoError blocking_finalize(void *context)
{
    assert(context == &sdk && !worker.accepting);
    assert_pending();
    assert(!pthread_mutex_lock(&phase_mutex));
    test_phase = 2;
    assert(!pthread_cond_broadcast(&phase_cond));
    while (!release_finalize)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    /* Published ordinary memory must be acquire-visible to the ACK reader
     * before pthread_join. This models the finalizer's completed effects. */
    finalized_payload = 0x1234;
    return VCO_E_RESTORE;
}
static void *thread_runner(void *unused)
{
    (void)unused;
    run_thread();
    return NULL;
}
static void *stop_requester(void *unused)
{
    (void)unused;
    assert(!vco_vita_control_worker_request_stop(&worker));
    return NULL;
}
static void shutdown_concurrent(void)
{
    setup();
    test_phase = finalized_payload = 0;
    release_operation = release_finalize = false;
    worker.finalize = blocking_finalize;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, blocking_update, &sdk));
    pthread_t owning_thread, stoppers[4];
    assert(!pthread_create(&owning_thread, NULL, thread_runner, NULL));
    assert(!pthread_mutex_lock(&phase_mutex));
    while (test_phase < 1)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&stoppers[i], NULL, stop_requester, NULL));
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_join(stoppers[i], NULL));
    assert_pending(); /* The admitted operation is still blocked. */
    assert(!sdk.waits && !sdk.deletes);
    assert(!pthread_mutex_lock(&phase_mutex));
    release_operation = true;
    assert(!pthread_cond_broadcast(&phase_cond));
    while (test_phase < 2)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    assert_pending(); /* Finalizer has started, but cannot finish yet. */
    assert(!vco_vita_control_worker_request_stop(&worker));
    assert(!pthread_mutex_lock(&phase_mutex));
    release_finalize = true;
    assert(!pthread_cond_broadcast(&phase_cond));
    assert(!pthread_mutex_unlock(&phase_mutex));
    VcoVitaControlShutdown out;
    while (!vco_vita_control_worker_shutdown(&worker, &out))
        sched_yield();
    assert(out.run_error == 0 && out.cleanup == VCO_E_RESTORE && out.mailbox_complete);
    assert(finalized_payload == 0x1234); /* Read before joining the native owner. */
    assert(!pthread_join(owning_thread, NULL));
    VcoError cleanup;
    assert(vco_vita_control_worker_stop(&worker, 1000, &cleanup) == VCO_VITA_WORKER_CLEANUP);
    assert(cleanup == VCO_E_RESTORE && worker.completed && !sdk.live && sdk.updates == 1);
}
static VcoCloseQueue scoped_queue;
static VcoEngineCloseKey scoped_key = {.identity = {42, 99, "PCSB00031"},
                                       .session = 1,
                                       .revision = 2,
                                       .transaction = 3,
                                       .scope = VCO_CLOSE_PROCESS,
                                       .process_object = 0x1234};
static uint64_t scoped_ticket;
static unsigned scoped_calls;
static VcoError scoped_execute(void *context, const VcoEngineCloseKey *key,
                               VcoEngineCloseResult *out)
{
    assert(context == &sdk && vco_close_key_equal(key, &scoped_key));
    ++scoped_calls;
    *out = (VcoEngineCloseResult){VCO_OK, VCO_E_RESTORE};
    return VCO_E_RESTORE;
}
static void setup_scoped(void)
{
    setup();
    scoped_calls = 0;
    vco_close_queue_init(&scoped_queue);
    worker.close_queue = &scoped_queue;
    worker.close_execute = scoped_execute;
}
static void scoped_delay(void)
{
    VcoVitaCloseResult result;
    assert(!vco_vita_control_worker_close_receive(&worker, sdk.delays == 1 ? 1 : 2, scoped_ticket,
                                                  &result));
    assert(result.target_complete && !result.owner_stopped &&
           result.target.error == VCO_E_RESTORE && result.target.cleanup.native == VCO_E_RESTORE);
    if (sdk.delays == 1) {
        uint64_t joined;
        assert(!sdk.updates && scoped_calls == 1);
        assert(!vco_vita_control_worker_close_submit(&worker, 2, &scoped_key, &joined));
        assert(joined == scoped_ticket);
        assert(!vco_vita_control_worker_close_release(&worker, 1, scoped_ticket));
    } else if (sdk.delays == 2) {
        assert(!sdk.updates && scoped_calls == 1);
        assert(!vco_vita_control_worker_close_release(&worker, 2, scoped_ticket));
        sdk.on_delay = NULL;
        sdk.delay_error = -106;
    } else
        assert(0);
}
static void scoped_close(void)
{
    setup_scoped();
    worker.close_execute = NULL;
    assert(vco_vita_control_worker_start(&worker, &mailbox, update, &sdk) ==
           VCO_VITA_WORKER_ARGUMENT);
    worker.close_execute = scoped_execute;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
    assert(!vco_vita_control_worker_close_submit(&worker, 1, &scoped_key, &scoped_ticket));
    sdk.on_delay = scoped_delay;
    run_thread();
    VcoVitaCloseResult result;
    assert(!sdk.updates && sdk.delays == 2 && scoped_calls == 1);
    assert(vco_vita_control_worker_close_receive(&worker, 1, scoped_ticket, &result) ==
           VCO_CLOSE_TICKET);
    assert(vco_vita_control_worker_close_submit(&worker, 3, &scoped_key, &scoped_ticket) ==
           VCO_CLOSE_CLOSED);
    assert(vco_vita_control_worker_stop(&worker, 1000, NULL) == -106);
}
static void scoped_fault(void)
{
    for (unsigned blocked = 0; blocked < 2; ++blocked) {
        setup_scoped();
        finalize_calls = 0;
        finalize_result = VCO_E_RESTORE;
        worker.finalize = finalize;
        assert(!vco_vita_control_worker_start(&worker, &mailbox, update, &sdk));
        assert(!vco_vita_control_worker_close_submit(&worker, 1, &scoped_key, &scoped_ticket));
        if (blocked)
            __atomic_store_n(&scoped_queue.gate, 1, __ATOMIC_RELEASE);
        sdk.now = -1;
        run_thread();
        assert(!sdk.updates && !sdk.delays && finalize_calls == 1);
        VcoVitaCloseResult result;
        memset(&result, 0x5a, sizeof(result));
        if (blocked) {
            VcoVitaCloseResult before = result;
            assert(vco_vita_control_worker_close_receive(&worker, 1, scoped_ticket, &result) ==
                   VCO_CLOSE_BUSY);
            assert(!memcmp(&result, &before, sizeof(result)));
            __atomic_store_n(&scoped_queue.gate, 0, __ATOMIC_RELEASE);
        }
        VcoCloseStatus status =
            vco_vita_control_worker_close_receive(&worker, 1, scoped_ticket, &result);
        assert(status == (blocked ? VCO_CLOSE_OWNER_STOPPED : VCO_CLOSE_OK));
        assert(result.target_complete == !blocked && result.owner_stopped &&
               result.shutdown.cleanup == VCO_E_RESTORE &&
               result.shutdown.run_error == VCO_VITA_WORKER_CLOCK);
        assert(scoped_calls == !blocked);
        assert(!vco_vita_control_worker_close_release(&worker, 1, scoped_ticket));
        assert(vco_vita_control_worker_stop(&worker, 1000, NULL) == VCO_VITA_WORKER_CLOCK);
    }
}
static void submit_during_update(void *context, uint64_t now)
{
    update(context, now);
    if (sdk.updates == 1)
        assert(!vco_vita_control_worker_close_submit(&worker, 1, &scoped_key, &scoped_ticket));
}
static void finish_native_operation(void)
{
    VcoVitaCloseResult result;
    VcoControlReply reply;
    assert(sdk.delays == 1 && sdk.updates == 1 && scoped_calls == 1);
    assert(!vco_vita_control_worker_close_receive(&worker, 1, scoped_ticket, &result));
    assert(vco_mailbox_receive(&mailbox, 88, transport_ticket, &reply) == VCO_MAILBOX_WAIT);
    assert(!vco_vita_control_worker_close_release(&worker, 1, scoped_ticket));
    sdk.on_delay = NULL;
}
static void stop_after_resume(void *context, uint64_t now)
{
    submit_during_update(context, now);
    if (sdk.updates == 2)
        assert(!vco_vita_control_worker_request_stop(&worker));
}
static bool scoped_terminal_release;
static VcoError concurrent_scoped_execute(void *context, const VcoEngineCloseKey *key,
                                          VcoEngineCloseResult *out)
{
    assert(context == &sdk && vco_close_key_equal(key, &scoped_key));
    ++scoped_calls;
    assert(!pthread_mutex_lock(&phase_mutex));
    test_phase = 2;
    assert(!pthread_cond_broadcast(&phase_cond));
    while (!release_finalize)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    finalized_payload = 0x1234;
    *out = (VcoEngineCloseResult){VCO_OK, VCO_E_IO};
    return VCO_E_IO;
}
static VcoError concurrent_scoped_finalize(void *context)
{
    assert(context == &sdk && !worker.accepting);
    assert(!pthread_mutex_lock(&phase_mutex));
    test_phase = 3;
    assert(!pthread_cond_broadcast(&phase_cond));
    while (!scoped_terminal_release)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    return VCO_E_RESTORE;
}
static void scoped_concurrent(void)
{
    setup_scoped();
    test_phase = finalized_payload = 0;
    release_operation = release_finalize = scoped_terminal_release = false;
    worker.close_execute = concurrent_scoped_execute;
    worker.finalize = concurrent_scoped_finalize;
    assert(!vco_vita_control_worker_start(&worker, &mailbox, blocking_update, &sdk));
    pthread_t owning, stoppers[4];
    assert(!pthread_create(&owning, NULL, thread_runner, NULL));
    assert(!pthread_mutex_lock(&phase_mutex));
    while (test_phase < 1)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    assert(!vco_vita_control_worker_close_submit(&worker, 1, &scoped_key, &scoped_ticket));
    VcoVitaCloseResult result;
    assert(vco_vita_control_worker_close_receive(&worker, 1, scoped_ticket, &result) ==
           VCO_CLOSE_WAIT);
    assert(!pthread_mutex_lock(&phase_mutex));
    release_operation = true;
    assert(!pthread_cond_broadcast(&phase_cond));
    while (test_phase < 2)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    uint64_t joined;
    assert(!vco_vita_control_worker_close_submit(&worker, 2, &scoped_key, &joined) &&
           joined == scoped_ticket);
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&stoppers[i], NULL, stop_requester, NULL));
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_join(stoppers[i], NULL));
    assert(vco_vita_control_worker_close_receive(&worker, 2, joined, &result) == VCO_CLOSE_WAIT);
    assert(vco_vita_control_worker_close_release(&worker, 1, scoped_ticket) == VCO_CLOSE_WAIT);
    assert(!pthread_mutex_lock(&phase_mutex));
    release_finalize = true;
    assert(!pthread_cond_broadcast(&phase_cond));
    while (test_phase < 3)
        assert(!pthread_cond_wait(&phase_cond, &phase_mutex));
    assert(!pthread_mutex_unlock(&phase_mutex));
    assert(!vco_vita_control_worker_close_receive(&worker, 1, scoped_ticket, &result));
    assert(result.target_complete && !result.owner_stopped && result.target.error == VCO_E_IO &&
           result.target.cleanup.native == VCO_E_IO && finalized_payload == 0x1234 &&
           scoped_calls == 1);
    assert(!vco_vita_control_worker_close_release(&worker, 1, scoped_ticket));
    assert(!pthread_mutex_lock(&phase_mutex));
    scoped_terminal_release = true;
    assert(!pthread_cond_broadcast(&phase_cond));
    assert(!pthread_mutex_unlock(&phase_mutex));
    VcoVitaControlShutdown terminal;
    while (!vco_vita_control_worker_shutdown(&worker, &terminal))
        sched_yield();
    assert(!vco_vita_control_worker_close_receive(&worker, 2, joined, &result));
    assert(result.target_complete && result.owner_stopped && result.target.error == VCO_E_IO &&
           result.shutdown.cleanup == VCO_E_RESTORE);
    assert(!vco_vita_control_worker_close_release(&worker, 2, joined));
    assert(!pthread_join(owning, NULL));
    assert(vco_vita_control_worker_stop(&worker, 1000, NULL) == VCO_VITA_WORKER_CLEANUP);
}
static void scoped_racing_update(void)
{
    setup_scoped();
    assert(!vco_vita_control_worker_start(&worker, &mailbox, stop_after_resume, &sdk));
    VcoControlRequest packet = request();
    assert(
        !vco_vita_control_worker_submit(&worker, 88, &packet, sizeof(packet), &transport_ticket));
    sdk.on_delay = finish_native_operation;
    run_thread();
    assert(sdk.updates == 2 && sdk.delays == 1 && scoped_calls == 1 && sdk.creates == 1);
    assert(!vco_vita_control_worker_stop(&worker, 1000, NULL));
}
static unsigned completion_mode, completion_calls, completed_native_payload;
static void scoped_completion(void *context, const VcoCloseReply *reply)
{
    assert(context == &sdk && vco_close_key_equal(&reply->key, &scoped_key) &&
           reply->error == VCO_E_RESTORE && !sdk.updates && completed_native_payload == 42);
    ++completion_calls;
    VcoControlReply pending;
    assert(vco_mailbox_receive(&mailbox, 88, transport_ticket, &pending) == VCO_MAILBOX_WAIT);
    uint64_t next;
    assert(vco_vita_control_worker_close_submit(&worker, 3, &scoped_key, &next) == VCO_CLOSE_BUSY);
    if (completion_mode == 2) {
        assert(!vco_vita_control_worker_request_stop(&worker));
        assert(!vco_vita_control_worker_running(&worker));
    }
}
static void completion_update(void *context, uint64_t now)
{
    assert(completion_calls == 1 && !completion_mode);
    update(context, now);
}
static void completion_delay(void)
{
    if (sdk.delays == 1) {
        VcoVitaCloseResult closed;
        assert(!vco_vita_control_worker_close_receive(&worker, 1, scoped_ticket, &closed));
        assert(closed.target.error == VCO_E_RESTORE && !completion_calls && !sdk.updates);
        completed_native_payload = 42;
        assert(!vco_vita_control_worker_close_release(&worker, 1, scoped_ticket));
        if (completion_mode == 1)
            assert(!vco_vita_control_worker_request_stop(&worker));
    } else {
        assert(sdk.delays == 2 && !completion_mode && completion_calls == 1 && sdk.updates == 1);
        VcoControlReply reply;
        assert(!vco_mailbox_receive(&mailbox, 88, transport_ticket, &reply) &&
               reply.status == VCO_CONTROL_UNBOUND);
        assert(!vco_mailbox_ack(&mailbox, 88, transport_ticket));
        assert(!vco_vita_control_worker_request_stop(&worker));
    }
}
static void scoped_finish(bool stop)
{
    for (unsigned mode = stop ? 1u : 0u; mode <= (stop ? 2u : 0u); ++mode) {
        setup_scoped();
        completion_mode = mode;
        completion_calls = completed_native_payload = 0;
        worker.close_finish = scoped_completion;
        assert(!vco_vita_control_worker_start(&worker, &mailbox, completion_update, &sdk));
        VcoControlRequest packet = request();
        assert(!vco_vita_control_worker_submit(&worker, 88, &packet, sizeof(packet),
                                               &transport_ticket));
        assert(!vco_vita_control_worker_close_submit(&worker, 1, &scoped_key, &scoped_ticket));
        sdk.on_delay = completion_delay;
        run_thread();
        assert(completion_calls == (mode != 1) && sdk.updates == (mode == 0) && scoped_calls == 1);
        assert(!vco_vita_control_worker_stop(&worker, 1000, NULL));
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "scoped_close"))
        scoped_close();
    else if (!strcmp(argv[1], "scoped_fault"))
        scoped_fault();
    else if (!strcmp(argv[1], "scoped_racing_update"))
        scoped_racing_update();
    else if (!strcmp(argv[1], "scoped_concurrent"))
        scoped_concurrent();
    else if (!strcmp(argv[1], "scoped_finish"))
        scoped_finish(false);
    else if (!strcmp(argv[1], "scoped_finish_stop"))
        scoped_finish(true);
    else if (!strcmp(argv[1], "finalization"))
        finalization();
    else if (!strcmp(argv[1], "async_stop"))
        async_stop();
    else if (!strcmp(argv[1], "shutdown_failures"))
        shutdown_failures();
    else if (!strcmp(argv[1], "shutdown_concurrent"))
        shutdown_concurrent();
    else if (!strcmp(argv[1], "shutdown_restart"))
        shutdown_restart();
    else if (!strcmp(argv[1], "lifecycle"))
        lifecycle();
    else if (!strcmp(argv[1], "failures"))
        failures();
    else if (!strcmp(argv[1], "transport"))
        transport();
    else if (!strcmp(argv[1], "reap_stopped"))
        reap_stopped();
    else
        assert(!"unknown test case");
    printf("native worker %s passed\n", argv[1]);
    return 0;
}
