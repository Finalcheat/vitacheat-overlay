#include "native_service.h"
#include "user_bridge.h"
#include "vita_allocator.h"
#include "vita_memory.h"
#include "vita_worker.h"
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>
/* No constructors, heap runtime or kernel references. The OS owns this storage
 * and terminates its threads with the game; taiHEN cleans target-PID patches. */
static VcoGamePool pool;
static VcoGameNativeMemory memory;
static VcoGameExecutor executor;
/* Keep a file-backed RW segment for the Vita loader, and an invalid thread ID
 * before module_start. An all-zero initializer leaves this SELF with only BSS. */
static VcoGameWorker worker = {.thread = -1};
static void *payload;
static bool entered, executor_ready;
static VcoAgentStatus poll(void *unused, uint64_t session, VcoAgentMessage *message, void *bytes,
                           size_t capacity)
{
    (void)unused;
    if (pool.poisoned)
        return VCO_AGENT_IO;
    return vco_game_bridge_poll(session, message, sizeof(*message), bytes, (uint32_t)capacity);
}
static VcoAgentStatus publish(void *unused, const VcoAgentResult *result)
{
    (void)unused;
    return vcoAgentPublish(result, sizeof(*result));
}
static VcoAgentStatus fault(void *unused, uint64_t session, VcoError error)
{
    (void)unused;
    return vcoAgentFault(session, pool.poisoned ? VCO_E_IO : error);
}
static VcoError free_storage(void)
{
    if (executor_ready && vco_game_executor_stop(&executor))
        return VCO_E_RESTORE;
    if (payload) {
        VcoAgentAllocator a = vco_game_pool_allocator(&pool);
        a.release(a.context, payload);
        payload = NULL;
    }
    return pool.initialized ? vco_game_pool_dispose(&pool) : VCO_OK;
}
int module_start(SceSize length, void *arguments)
{
    if (entered)
        return SCE_KERNEL_START_SUCCESS;
    if (!arguments || length != sizeof(VcoAgentBinding))
        return SCE_KERNEL_START_FAILED;
    entered = true;
    bool allocation_failed = false;
    VcoAgentBinding binding;
    memcpy(&binding, arguments, sizeof(binding));
    vco_game_worker_init(&worker);
    int error;
    unsigned attach_attempt = 0;
    do {
        error = vcoAgentAttach(&binding, sizeof(binding));
    } while (error == VCO_AGENT_BUSY && attach_attempt++ < 7u && sceKernelDelayThread(1000u) >= 0);
    if (!error)
        error = vco_game_pool_init(&pool);
    if (!error)
        error = vco_game_native_memory_init(&memory, &binding);
    if (!error) {
        error = vco_game_executor_init(&executor, &binding, vco_game_pool_allocator(&pool),
                                       vco_game_native_memory_callbacks(&memory),
                                       vco_game_native_memory_finalize, &memory);
        executor_ready = !error;
    }
    if (!error)
        error = vco_game_executor_set_prepare(&executor, vco_game_native_memory_prepare, &memory);
    if (!error) {
        VcoAgentAllocator a = vco_game_pool_allocator(&pool);
        payload = a.allocate(a.context, VCO_UPLOAD_MAX);
        if (!payload) {
            allocation_failed = true;
            error = VCO_E_LIMIT;
        }
    }
    if (!error)
        error =
            vco_game_worker_start(&worker, &executor, (VcoGameChannel){NULL, poll, publish, fault},
                                  payload, VCO_UPLOAD_MAX);
    if (!error)
        return SCE_KERNEL_START_SUCCESS;
    if (worker.thread >= 0) {
        /* A failed DeleteThread leaves its entry pointing into this module.
         * Returning START_FAILED could auto-unload live code. Stay resident,
         * report the fault and refuse stop until the UID is actually gone. */
        (void)vcoAgentFault(binding.session, VCO_E_IO);
        return SCE_KERNEL_START_SUCCESS;
    }
    VcoError failure = allocation_failed ? VCO_E_LIMIT : VCO_E_IO;
    VcoError cleanup = free_storage();
    (void)vcoAgentFault(binding.session, cleanup ? cleanup : failure);
    /* No thread/patch can point into this module; process-owned data from an
     * uncertain SDK free, if any, is reclaimed at process exit. */
    return SCE_KERNEL_START_FAILED;
}
int module_stop(SceSize length, void *arguments)
{
    (void)length;
    (void)arguments;
    if (!entered)
        return SCE_KERNEL_STOP_SUCCESS;
    vco_game_worker_request_stop(&worker);
    /* module_stop can run under module-manager locks. Never wait for a worker
     * that still needs SDK calls: cancel this attempt, retry after it completes. */
    if (worker.started && !__atomic_load_n(&worker.completed, __ATOMIC_ACQUIRE))
        return SCE_KERNEL_STOP_CANCEL;
    (void)vco_game_worker_stop(&worker, 1000u);
    if (worker.thread >= 0 || (worker.started && executor_ready && !executor.stopped) ||
        free_storage())
        return SCE_KERNEL_STOP_CANCEL;
    return SCE_KERNEL_STOP_SUCCESS;
}
