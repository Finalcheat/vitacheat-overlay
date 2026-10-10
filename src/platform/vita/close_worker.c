#include "control_worker.h"

VcoCloseStatus vco_vita_control_worker_close_submit(VcoVitaControlWorker *worker,
                                                    uint64_t requester,
                                                    const VcoEngineCloseKey *key, uint64_t *ticket)
{
    if (!worker || !worker->close_queue)
        return VCO_CLOSE_ARGUMENT;
    if (!vco_vita_control_worker_running(worker))
        return VCO_CLOSE_CLOSED;
    return vco_close_queue_submit(worker->close_queue, requester, key, ticket);
}
VcoCloseStatus vco_vita_control_worker_close_receive(const VcoVitaControlWorker *worker,
                                                     uint64_t requester, uint64_t ticket,
                                                     VcoVitaCloseResult *out)
{
    if (!worker || !worker->close_queue || !out)
        return VCO_CLOSE_ARGUMENT;
    VcoVitaCloseResult result = {0};
    VcoCloseStatus status =
        vco_close_queue_receive(worker->close_queue, requester, ticket, &result.target);
    if (status == VCO_CLOSE_OK || status == VCO_CLOSE_WAIT) {
        result.owner_stopped = vco_vita_control_worker_shutdown(worker, &result.shutdown) == 1;
        result.target_complete = status == VCO_CLOSE_OK;
        if (status == VCO_CLOSE_WAIT && result.owner_stopped)
            status = VCO_CLOSE_OWNER_STOPPED;
        if (status == VCO_CLOSE_OK || status == VCO_CLOSE_OWNER_STOPPED)
            *out = result;
    }
    return status;
}
VcoCloseStatus vco_vita_control_worker_close_release(const VcoVitaControlWorker *worker,
                                                     uint64_t requester, uint64_t ticket)
{
    if (!worker || !worker->close_queue)
        return VCO_CLOSE_ARGUMENT;
    VcoCloseStatus result = vco_close_queue_release(worker->close_queue, requester, ticket);
    if (result == VCO_CLOSE_WAIT) {
        VcoVitaControlShutdown terminal;
        if (vco_vita_control_worker_shutdown(worker, &terminal) == 1)
            result = vco_close_queue_abandon_after_stop(worker->close_queue, requester, ticket);
    }
    return result;
}
