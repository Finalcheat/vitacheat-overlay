#ifndef VCO_VITA_CONTROL_WORKER_H
#define VCO_VITA_CONTROL_WORKER_H
#include "../../execution/mailbox.h"
#include "../../execution/close_queue.h"

#define VCO_VITA_WORKER_ARGUMENT (-1)
#define VCO_VITA_WORKER_STATE (-2)
#define VCO_VITA_WORKER_CLEANUP (-3)
#define VCO_VITA_WORKER_CLOCK (-4)
#define VCO_VITA_WORKER_PERIOD_US 5000u

/* Called only on the worker, before dispatch and execution. Future native
 * session observation belongs here; it must not block indefinitely. */
typedef void (*VcoVitaControlUpdate)(void *context, uint64_t now_us);
/* Configure before start. Runs once on the owning worker after mailbox cleanup,
 * including clock/delay failure, before thread exit. Never runs on failed start.
 * Must not re-enable execution or discard uncertain native ownership. */
typedef VcoError (*VcoVitaControlFinalize)(void *context);
/* Immutable acknowledgement published by the owning worker only after its
 * mailbox cleanup and native finalizer have returned. It proves no future
 * engine/native access by that worker, not native thread deletion, successful
 * restoration, target lifetime, or permission to destroy a process/module. */
typedef struct {
    int run_error;
    VcoError cleanup;
    bool mailbox_complete;
} VcoVitaControlShutdown;
typedef struct {
    VcoCloseReply target;
    VcoVitaControlShutdown shutdown;
    bool target_complete, owner_stopped;
} VcoVitaCloseResult;
typedef struct {
    VcoControlMailbox *mailbox;
    VcoVitaControlUpdate update;
    VcoVitaControlFinalize finalize;
    void *context;
    /* Optional pair, immutable before start. Callback runs on this worker,
     * outside queue gates and before update/mailbox work. Its context is the
     * same resident context used by update/finalize. */
    VcoCloseQueue *close_queue;
    VcoCloseExecute close_execute;
    /* Optional post-native completion on this same owning worker. */
    VcoCloseFinish close_finish;
    int32_t thread;
    uint32_t stop_requested, accepting;
    uint32_t lifecycle;
    VcoVitaControlShutdown shutdown;
    int run_error;
    VcoError cleanup, finalize_error;
    bool started, joined, completed;
} VcoVitaControlWorker;

/* Initialize once. Lifecycle calls are externally serialized and producer
 * callers, shutdown requesters and acknowledgement readers must be quiescent
 * before initialization/restart. Worker, mailbox, engine, callback context and
 * close queue and all engine arenas must outlive both a successful join and all
 * in-flight requester/reader/producer calls. Cleanup failure retains ownership of that
 * context and forbids restart;
 * deleting the thread alone does not resolve quarantined native resources.
 * The resident service owns this lifecycle; no worker is started by UI hooks. */
void vco_vita_control_worker_init(VcoVitaControlWorker *);
/* Mailbox must already be initialized. A successful start transfers exclusive
 * engine ownership to the worker. Failed start leaves that ownership with the
 * caller, even if deleting the unstarted thread also fails. */
int vco_vita_control_worker_start(VcoVitaControlWorker *, VcoControlMailbox *,
                                  VcoVitaControlUpdate update, void *context);
/* Resident trusted callers only, after successful start. No SDK call, wait,
 * engine access, or Shell authentication is performed. Concurrent requests
 * coalesce into the same irreversible stop; admitted work may still finish.
 * Returns 0 when requested/already requested/quiescent, STATE before start,
 * ARGUMENT for NULL. Closing is the transport admission linearization point.
 * This stops the WHOLE worker, not a PID-specific lifecycle transaction. */
int vco_vita_control_worker_request_stop(VcoVitaControlWorker *);
/* Nonblocking acquire-read: 1 with a complete immutable snapshot, 0 while no
 * acknowledgement exists, ARGUMENT for NULL. Pending/error leaves *out intact.
 * No clock, SDK, mailbox, engine or Shell calls. Any restoration error remains
 * in cleanup. Even success does not transfer ownership: join+delete and caller
 * quiescence are still required. Missing ACK is never permission to proceed. */
int vco_vita_control_worker_shutdown(const VcoVitaControlWorker *, VcoVitaControlShutdown *out);
/* Observation only; producer APIs still arbitrate their own admission. */
int vco_vita_control_worker_running(const VcoVitaControlWorker *);
/* Trusted resident calls; no Shell identity, SDK, allocation or waiting. Native
 * callers must independently hold verified object/mapping leases and establish
 * a legal wait context before using this handshake at a destructive boundary.
 * Pending results leave *out untouched. OWNER_STOPPED reports the immutable
 * whole-worker ACK, NOT target cleanup success; target_complete remains false.
 * Release is after native completion/cancellation, and permits cancellation of
 * an unserved request only after an acquire-observed terminal worker ACK. */
VcoCloseStatus vco_vita_control_worker_close_submit(VcoVitaControlWorker *, uint64_t requester,
                                                    const VcoEngineCloseKey *, uint64_t *ticket);
VcoCloseStatus vco_vita_control_worker_close_receive(const VcoVitaControlWorker *,
                                                     uint64_t requester, uint64_t ticket,
                                                     VcoVitaCloseResult *out);
VcoCloseStatus vco_vita_control_worker_close_release(const VcoVitaControlWorker *,
                                                     uint64_t requester, uint64_t ticket);
/* Timeout is bounded by the caller, in microseconds. Never forcibly terminates
 * a live thread. Failed join/delete retains its handle for a later stop retry.
 * Zero means joined, deleted and restored; nonzero cleanup is returned through
 * cleanup and VCO_VITA_WORKER_CLEANUP. Other negative values are native thread
 * errors or wrapper errors. A cleanup failure still forbids releasing engine
 * and native-memory ownership, even after the thread is successfully deleted.
 * Requesting stop can race an already-started update/engine step; only completed
 * cleanup ACK establishes no further writes by this worker; a successful join
 * and caller quiescence are additionally required for ownership handoff.
 * Before start (or after a failed start), stop only releases a dormant thread;
 * it does not unbind the caller-owned mailbox or engine. */
int vco_vita_control_worker_stop(VcoVitaControlWorker *, uint32_t timeout_us, VcoError *cleanup);
/* Externally serialized, nonblocking retirement of a worker which has already
 * stopped accepting. Zero proves native thread termination/deletion AND a
 * completed mailbox cleanup attempt; cleanup may still be RESTORE and must be
 * retained. Never requests stop on an accepting worker or grants engine access.
 * Failed wait/delete may be retried. Uses WaitThreadEnd with timeout zero. */
int vco_vita_control_worker_reap_stopped(VcoVitaControlWorker *, VcoError *cleanup);

/* Nonblocking calls with kernel-owned packet buffers only. Syscall adapters
 * still need trusted caller identity and CopyFromUser/CopyToUser. A successful
 * receive retains the reply until ack, allowing CopyToUser failures to retry. */
VcoMailboxStatus vco_vita_control_worker_submit(VcoVitaControlWorker *, int32_t caller,
                                                const void *packet, size_t length,
                                                uint64_t *ticket);
VcoMailboxStatus vco_vita_control_worker_receive(VcoVitaControlWorker *, int32_t caller,
                                                 uint64_t ticket, VcoControlReply *reply);
VcoMailboxStatus vco_vita_control_worker_ack(VcoVitaControlWorker *, int32_t caller,
                                             uint64_t ticket);
/* Recover an outstanding request/ticket after a client restart, then receive
 * and acknowledge it before issuing a fresh client query. Does not cancel it. */
VcoMailboxStatus vco_vita_control_worker_resume(VcoVitaControlWorker *, int32_t caller,
                                                VcoControlRequest *request, uint64_t *ticket);
#endif
