#include "vita_worker.h"
#include <psp2/kernel/threadmgr.h>
#include <string.h>

void vco_game_worker_init(VcoGameWorker *w)
{
    if (w) {
        memset(w, 0, sizeof(*w));
        w->thread = -1;
    }
}
void vco_game_worker_request_stop(VcoGameWorker *w)
{
    if (w)
        __atomic_store_n(&w->stop_requested, 1u, __ATOMIC_RELEASE);
}
static int entry(SceSize length, void *arguments)
{
    VcoGameWorker *w = NULL;
    if (!arguments || length != sizeof(w))
        return VCO_GAME_WORKER_ARGUMENT;
    memcpy(&w, arguments, sizeof(w));
    if (!w)
        return VCO_GAME_WORKER_ARGUMENT;
    while (!__atomic_load_n(&w->stop_requested, __ATOMIC_ACQUIRE)) {
        VcoAgentMessage message;
        VcoAgentStatus status = w->channel.poll(w->channel.context, w->executor->binding.session,
                                                &message, w->payload, w->capacity);
        if (status == VCO_AGENT_STALE || status == VCO_AGENT_OWNER || status == VCO_AGENT_CLOSED) {
            /* Session loss has no restore attempt. Termination cleanup belongs
             * to OS/taiHEN, not a callback writing into a dying address space. */
            vco_game_executor_revoke(w->executor);
            w->run_error = VCO_GAME_WORKER_SESSION;
            break;
        }
        if (status == VCO_AGENT_OK) {
            if (__atomic_load_n(&w->stop_requested, __ATOMIC_ACQUIRE))
                break;
            SceInt64 clock = sceKernelGetSystemTimeWide();
            if (clock < 0) {
                w->run_error = VCO_GAME_WORKER_CLOCK;
                break;
            }
            VcoAgentResult result;
            VcoError error = vco_game_executor_handle(w->executor, &message, w->payload,
                                                      message.length, (uint64_t)clock, &result);
            if (error) {
                w->run_error = VCO_GAME_WORKER_PROTOCOL;
                break;
            }
            if (message.ticket) {
                status = w->channel.publish(w->channel.context, &result);
                if (status == VCO_AGENT_STALE || status == VCO_AGENT_OWNER ||
                    status == VCO_AGENT_CLOSED) {
                    vco_game_executor_revoke(w->executor);
                    w->run_error = VCO_GAME_WORKER_SESSION;
                    break;
                }
                if (status != VCO_AGENT_OK && status != VCO_AGENT_BUSY &&
                    status != VCO_AGENT_COPY) {
                    w->run_error = VCO_GAME_WORKER_PROTOCOL;
                    break;
                }
                /* BUSY leaves the receipt pending. Repeated poll uses the
                 * executor's cached result and never reruns an operation. */
            }
        } else if (status != VCO_AGENT_BUSY && status != VCO_AGENT_EMPTY &&
                   status != VCO_AGENT_COPY) {
            w->run_error = VCO_GAME_WORKER_PROTOCOL;
            break;
        }
        /* Transient poll failure never ticks using yesterday's authority. */
        int delay = sceKernelDelayThread(VCO_GAME_WORKER_PERIOD_US);
        if (delay < 0) {
            w->run_error = delay;
            break;
        }
    }
    w->cleanup = w->executor->revoked ? VCO_E_SESSION : vco_game_executor_stop(w->executor);
    if (!w->executor->revoked && w->channel.fault && (w->run_error || w->cleanup)) {
        VcoError error = w->cleanup ? w->cleanup : VCO_E_IO;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            if (w->channel.fault(w->channel.context, w->executor->binding.session, error) !=
                VCO_AGENT_BUSY)
                break;
            if (sceKernelDelayThread(1000u) < 0)
                break;
        }
    }
    if (!w->executor->revoked && !w->cleanup) {
        VcoAgentResult stopped;
        VcoAgentStatus delivered = VCO_AGENT_ARGUMENT;
        if (!vco_game_executor_stopped_result(w->executor, &stopped)) {
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                delivered = w->channel.publish(w->channel.context, &stopped);
                if (delivered != VCO_AGENT_BUSY && delivered != VCO_AGENT_COPY)
                    break;
                if (sceKernelDelayThread(1000u) < 0)
                    break;
            }
        }
        if (delivered != VCO_AGENT_OK && !w->run_error) {
            w->run_error = VCO_GAME_WORKER_PROTOCOL;
            if (w->channel.fault)
                (void)w->channel.fault(w->channel.context, w->executor->binding.session, VCO_E_IO);
        }
    }
    __atomic_store_n(&w->completed, 1u, __ATOMIC_RELEASE);
    return w->run_error;
}
int vco_game_worker_start(VcoGameWorker *w, VcoGameExecutor *g, VcoGameChannel channel,
                          void *payload, size_t capacity)
{
    if (!w || !g || !channel.poll || !channel.publish || !payload || capacity < VCO_UPLOAD_MAX)
        return VCO_GAME_WORKER_ARGUMENT;
    if (w->thread >= 0 || w->started || g->revoked || g->stopped)
        return VCO_GAME_WORKER_STATE;
    w->executor = g;
    w->channel = channel;
    w->payload = payload;
    w->capacity = capacity;
    w->cleanup = VCO_OK;
    w->run_error = 0;
    __atomic_store_n(&w->stop_requested, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&w->completed, 0u, __ATOMIC_RELEASE);
    int thread = sceKernelCreateThread("vco_game_execution", entry, 0x10000100, 0x6000, 0, 0, NULL);
    if (thread < 0)
        return thread;
    w->thread = thread;
    VcoGameWorker *argument = w;
    int result = sceKernelStartThread(thread, sizeof(argument), &argument);
    if (result < 0) {
        if (sceKernelDeleteThread(thread) >= 0)
            w->thread = -1;
        return result;
    }
    w->started = true;
    return 0;
}
int vco_game_worker_stop(VcoGameWorker *w, uint32_t timeout_us)
{
    if (!w)
        return VCO_GAME_WORKER_ARGUMENT;
    if (w->thread < 0)
        return w->run_error ? w->run_error : w->cleanup ? VCO_GAME_WORKER_CLEANUP : 0;
    if (w->started && !w->joined) {
        vco_game_worker_request_stop(w);
        SceUInt timeout = timeout_us;
        int result = sceKernelWaitThreadEnd(w->thread, &w->run_error, &timeout);
        if (result < 0)
            return result;
        w->joined = true;
        if (!__atomic_load_n(&w->completed, __ATOMIC_ACQUIRE))
            w->run_error = VCO_GAME_WORKER_STATE;
    }
    int result = sceKernelDeleteThread(w->thread);
    if (result < 0)
        return result;
    w->thread = -1;
    return w->run_error ? w->run_error : w->cleanup ? VCO_GAME_WORKER_CLEANUP : 0;
}
