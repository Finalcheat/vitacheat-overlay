#include "control_worker.h"
#include <psp2kern/kernel/threadmgr.h>
#include <string.h>

#if !defined(__GCC_ATOMIC_INT_LOCK_FREE) || __GCC_ATOMIC_INT_LOCK_FREE != 2
#error "Worker shutdown requires lock-free 32-bit integer atomics"
#endif
enum { NOT_STARTED, RUNNING, CLOSING, QUIESCENT };
typedef char shutdown_atomic_size_check[sizeof(unsigned int) == sizeof(uint32_t) ? 1 : -1];
int vco_vita_control_worker_running(const VcoVitaControlWorker *worker)
{
    return worker && __atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE) == RUNNING &&
           __atomic_load_n(&worker->accepting, __ATOMIC_ACQUIRE);
}

int vco_vita_control_worker_request_stop(VcoVitaControlWorker *worker)
{
    if (!worker)
        return VCO_VITA_WORKER_ARGUMENT;
    uint32_t state = __atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE);
    for (;;) {
        if (state == NOT_STARTED)
            return VCO_VITA_WORKER_STATE;
        if (state == QUIESCENT)
            return 0;
        if (state == CLOSING ||
            __atomic_compare_exchange_n(&worker->lifecycle, &state, CLOSING, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }
    __atomic_store_n(&worker->accepting, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&worker->stop_requested, 1u, __ATOMIC_RELEASE);
    vco_close_queue_stop(worker->close_queue);
    vco_mailbox_stop(worker->mailbox);
    return 0;
}

int vco_vita_control_worker_shutdown(const VcoVitaControlWorker *worker,
                                     VcoVitaControlShutdown *out)
{
    if (!worker || !out)
        return VCO_VITA_WORKER_ARGUMENT;
    if (__atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE) != QUIESCENT)
        return 0;
    *out = worker->shutdown;
    return 1;
}

static int worker_entry(SceSize length, void *argument)
{
    VcoVitaControlWorker *worker = NULL;
    if (!argument || length != sizeof(worker))
        return VCO_VITA_WORKER_ARGUMENT;
    /* StartThread copies the pointer value, not the worker object. */
    memcpy(&worker, argument, sizeof(worker));
    if (!worker)
        return VCO_VITA_WORKER_ARGUMENT;
    for (;;) {
        SceInt64 clock = ksceKernelGetSystemTimeWide();
        if (clock < 0) {
            worker->run_error = VCO_VITA_WORKER_CLOCK;
            (void)vco_vita_control_worker_request_stop(worker);
        }
        uint64_t now = clock < 0 ? 0 : (uint64_t)clock;
        /* A requester can be preempted after closing lifecycle but before its
         * mailbox store. Finish the same stop here rather than run new work. */
        if (__atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE) == CLOSING)
            (void)vco_vita_control_worker_request_stop(worker);
        /* Scoped cleanup precedes all Shell-dependent work. Accepted peers
         * retain native protection until ACK AND native completion; park update
         * and dispatch throughout that interval. A request racing update is
         * checked again before the mailbox can begin another execution round. */
        bool parked = vco_close_queue_pump(
            worker->close_queue, worker->close_execute,
            vco_vita_control_worker_running(worker) ? worker->close_finish : NULL, worker->context);
        if (!parked && __atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE) == RUNNING &&
            !__atomic_load_n(&worker->stop_requested, __ATOMIC_ACQUIRE) &&
            vco_mailbox_open(worker->mailbox) && worker->update)
            worker->update(worker->context, now);
        if (!parked)
            parked = vco_close_queue_pump(
                worker->close_queue, worker->close_execute,
                vco_vita_control_worker_running(worker) ? worker->close_finish : NULL,
                worker->context);
        /* Whole-worker stop still drains/finalizes, even while scoped peers
         * hold their ACK. It is independent of their release and of Shell. */
        if ((!parked || !vco_mailbox_open(worker->mailbox)) &&
            !vco_mailbox_pump(worker->mailbox, now))
            break;
        int result = ksceKernelDelayThread(VCO_VITA_WORKER_PERIOD_US);
        if (result < 0) {
            worker->run_error = result;
            (void)vco_vita_control_worker_request_stop(worker);
        }
    }
    __atomic_store_n(&worker->accepting, 0u, __ATOMIC_RELEASE);
    vco_close_queue_stop(worker->close_queue);
    if (worker->finalize)
        worker->finalize_error = worker->finalize(worker->context);
    VcoVitaControlShutdown result = {.run_error = worker->run_error, .cleanup = VCO_OK};
    result.mailbox_complete = vco_mailbox_stop_result(worker->mailbox, &result.cleanup) != 0;
    if (!result.mailbox_complete)
        result.run_error = VCO_VITA_WORKER_STATE;
    if (result.cleanup == VCO_OK)
        result.cleanup = worker->finalize_error;
    worker->shutdown = result;
    /* This is the last access to engine/mailbox/close-queue/finalizer state. A reader may
     * observe the ACK before the native thread actually returns. No rebind,
     * initialization or resource handoff is allowed on that observation alone. */
    __atomic_store_n(&worker->lifecycle, QUIESCENT, __ATOMIC_RELEASE);
    return result.run_error;
}

void vco_vita_control_worker_init(VcoVitaControlWorker *worker)
{
    if (!worker)
        return;
    memset(worker, 0, sizeof(*worker));
    worker->thread = -1;
}

int vco_vita_control_worker_start(VcoVitaControlWorker *worker, VcoControlMailbox *mailbox,
                                  VcoVitaControlUpdate update, void *context)
{
    if (!worker || !mailbox)
        return VCO_VITA_WORKER_ARGUMENT;
    if (!!worker->close_queue != !!worker->close_execute ||
        (!worker->close_queue && worker->close_finish))
        return VCO_VITA_WORKER_ARGUMENT;
    if (worker->close_queue && !vco_close_queue_open(worker->close_queue))
        return VCO_VITA_WORKER_STATE;
    if (worker->thread >= 0)
        return VCO_VITA_WORKER_STATE;
    if (worker->started && (!worker->completed || worker->cleanup != VCO_OK))
        return VCO_VITA_WORKER_STATE;
    if (!vco_mailbox_open(mailbox))
        return VCO_VITA_WORKER_STATE;
    /* Restart is permitted only with every old requester/reader quiescent.
     * Unpublish the prior ACK before replacing its worker-owned context, even
     * if CreateThread subsequently fails and no new thread exists. */
    __atomic_store_n(&worker->lifecycle, NOT_STARTED, __ATOMIC_RELEASE);
    worker->mailbox = mailbox;
    worker->update = update;
    worker->context = context;
    worker->started = worker->joined = worker->completed = false;
    worker->run_error = 0;
    worker->cleanup = worker->finalize_error = VCO_OK;
    __atomic_store_n(&worker->stop_requested, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&worker->accepting, 0u, __ATOMIC_RELEASE);
    int thread =
        ksceKernelCreateThread("vco_execution", worker_entry, 0x10000100, 0x4000, 0, 0, NULL);
    if (thread < 0)
        return thread;
    worker->thread = thread;
    __atomic_store_n(&worker->accepting, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&worker->lifecycle, RUNNING, __ATOMIC_RELEASE);
    VcoVitaControlWorker *argument = worker;
    int result = ksceKernelStartThread(thread, sizeof(argument), &argument);
    if (result < 0) {
        __atomic_store_n(&worker->accepting, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&worker->lifecycle, NOT_STARTED, __ATOMIC_RELEASE);
        if (ksceKernelDeleteThread(thread) >= 0)
            worker->thread = -1;
        return result;
    }
    worker->started = true;
    return 0;
}

static int completion(const VcoVitaControlWorker *worker, VcoError *cleanup)
{
    if (cleanup)
        *cleanup = worker->cleanup;
    if (worker->run_error)
        return worker->run_error;
    return worker->cleanup != VCO_OK ? VCO_VITA_WORKER_CLEANUP : 0;
}

int vco_vita_control_worker_stop(VcoVitaControlWorker *worker, uint32_t timeout_us,
                                 VcoError *cleanup)
{
    if (!worker)
        return VCO_VITA_WORKER_ARGUMENT;
    if (cleanup)
        *cleanup = VCO_OK;
    if (worker->thread < 0)
        return completion(worker, cleanup);
    __atomic_store_n(&worker->accepting, 0u, __ATOMIC_RELEASE);
    if (worker->started && !worker->joined) {
        (void)vco_vita_control_worker_request_stop(worker);
        SceUInt timeout = timeout_us;
        int exit_status = 0;
        int result = ksceKernelWaitThreadEnd(worker->thread, &exit_status, &timeout);
        if (result < 0)
            return result;
        worker->joined = true;
        worker->run_error = exit_status;
        VcoVitaControlShutdown acknowledged;
        bool has_ack = vco_vita_control_worker_shutdown(worker, &acknowledged) == 1;
        if (has_ack)
            worker->cleanup = acknowledged.cleanup;
        worker->completed =
            has_ack && acknowledged.mailbox_complete && acknowledged.run_error == exit_status;
        if (!worker->completed)
            worker->run_error = VCO_VITA_WORKER_STATE;
    }
    if (cleanup)
        *cleanup = worker->cleanup;
    int result = ksceKernelDeleteThread(worker->thread);
    if (result < 0)
        return result;
    worker->thread = -1;
    return completion(worker, cleanup);
}

int vco_vita_control_worker_reap_stopped(VcoVitaControlWorker *worker, VcoError *cleanup)
{
    if (!worker || __atomic_load_n(&worker->accepting, __ATOMIC_ACQUIRE) || !worker->started)
        return VCO_VITA_WORKER_STATE;
    int result = vco_vita_control_worker_stop(worker, 0, cleanup);
    if (worker->joined && worker->completed && worker->thread < 0)
        return 0;
    return result ? result : VCO_VITA_WORKER_STATE;
}
VcoMailboxStatus vco_vita_control_worker_submit(VcoVitaControlWorker *worker, int32_t caller,
                                                const void *packet, size_t length, uint64_t *ticket)
{
    if (!worker)
        return VCO_MAILBOX_ARGUMENT;
    if (__atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE) != RUNNING ||
        !__atomic_load_n(&worker->accepting, __ATOMIC_ACQUIRE))
        return VCO_MAILBOX_CLOSED;
    return vco_mailbox_submit(worker->mailbox, caller, packet, length, ticket);
}

VcoMailboxStatus vco_vita_control_worker_receive(VcoVitaControlWorker *worker, int32_t caller,
                                                 uint64_t ticket, VcoControlReply *reply)
{
    if (!worker)
        return VCO_MAILBOX_ARGUMENT;
    if (__atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE) != RUNNING ||
        !__atomic_load_n(&worker->accepting, __ATOMIC_ACQUIRE))
        return VCO_MAILBOX_CLOSED;
    return vco_mailbox_receive(worker->mailbox, caller, ticket, reply);
}

VcoMailboxStatus vco_vita_control_worker_ack(VcoVitaControlWorker *worker, int32_t caller,
                                             uint64_t ticket)
{
    if (!worker)
        return VCO_MAILBOX_ARGUMENT;
    if (__atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE) != RUNNING ||
        !__atomic_load_n(&worker->accepting, __ATOMIC_ACQUIRE))
        return VCO_MAILBOX_CLOSED;
    return vco_mailbox_ack(worker->mailbox, caller, ticket);
}

VcoMailboxStatus vco_vita_control_worker_resume(VcoVitaControlWorker *worker, int32_t caller,
                                                VcoControlRequest *request, uint64_t *ticket)
{
    if (!worker)
        return VCO_MAILBOX_ARGUMENT;
    if (__atomic_load_n(&worker->lifecycle, __ATOMIC_ACQUIRE) != RUNNING ||
        !__atomic_load_n(&worker->accepting, __ATOMIC_ACQUIRE))
        return VCO_MAILBOX_CLOSED;
    return vco_mailbox_resume(worker->mailbox, caller, request, ticket);
}
