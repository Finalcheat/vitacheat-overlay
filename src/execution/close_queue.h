#ifndef VCO_CLOSE_QUEUE_H
#define VCO_CLOSE_QUEUE_H
#include "close_key.h"
#define VCO_CLOSE_PEERS 8u
typedef enum {
    VCO_CLOSE_OK,
    VCO_CLOSE_BUSY,
    VCO_CLOSE_WAIT,
    VCO_CLOSE_TICKET,
    VCO_CLOSE_CLOSED,
    VCO_CLOSE_ARGUMENT,
    VCO_CLOSE_EXHAUSTED,
    VCO_CLOSE_LIMIT,
    VCO_CLOSE_OWNER_STOPPED
} VcoCloseStatus;
typedef struct {
    uint64_t ticket;
    VcoEngineCloseKey key;
    VcoError error;
    VcoEngineCloseResult cleanup;
} VcoCloseReply;
typedef VcoError (*VcoCloseExecute)(void *, const VcoEngineCloseKey *, VcoEngineCloseResult *);
/* Owning worker, once after every joined invocation ends its native operation.
 * The original leases/tags may already be retired; never dereference them.
 * Independently resample current native identity/metadata before reopening.
 * Completion errors belong to the owning service, not the immutable close ACK.
 * No waiting for this callback is performed by release(). */
typedef void (*VcoCloseFinish)(void *, const VcoCloseReply *);
typedef struct {
    uint64_t requester;
    bool released;
} VcoClosePeer;
/* Single owning worker, multiple trusted resident producers. One object
 * transaction, up to eight distinct joining invocations. No allocation, SDK,
 * waits or callbacks under the try-once gate. Fields are private to this API. */
typedef struct {
    uint32_t gate, state, closed;
    unsigned active, used;
    uint64_t sequence, ticket;
    VcoEngineCloseKey key;
    VcoCloseReply reply;
    VcoClosePeer peers[VCO_CLOSE_PEERS];
} VcoCloseQueue;
/* Before publication only. Storage, providers and all retained object/mapping
 * leases must outlive both worker termination and every producer/reader call.
 * No reinit until all potential callers are quiescent. */
void vco_close_queue_init(VcoCloseQueue *);
int vco_close_queue_open(const VcoCloseQueue *);
void vco_close_queue_stop(VcoCloseQueue *);
/* requester identifies a unique invocation while its ticket is retained, not
 * just a recycled thread UID. Duplicate submit by that invocation is idempotent.
 * Different keys return BUSY: never let native destruction proceed on rejection.
 * An accepted caller retains independently validated objects/mapping protection
 * through ACK and its native operation; no pointers are acquired by this queue.
 * Acceptance is NOT a drain ACK: previously admitted worker work may finish. */
VcoCloseStatus vco_close_queue_submit(VcoCloseQueue *, uint64_t requester,
                                      const VcoEngineCloseKey *, uint64_t *ticket);
VcoCloseStatus vco_close_queue_receive(VcoCloseQueue *, uint64_t requester, uint64_t ticket,
                                       VcoCloseReply *);
/* Only after ACK AND the native operation/cancellation has finished. Every
 * joined invocation releases once. Other peers keep the owner parked. The last
 * release schedules worker completion before this slot can accept a new key. */
VcoCloseStatus vco_close_queue_release(VcoCloseQueue *, uint64_t requester, uint64_t ticket);
/* Only after independently observing that the owner has irrevocably stopped
 * accessing the queue (not merely that admission is closed). Allows an unserved
 * invocation to cancel its lease; it never manufactures a cleanup ACK. */
VcoCloseStatus vco_close_queue_abandon_after_stop(VcoCloseQueue *, uint64_t requester,
                                                  uint64_t ticket);
/* Owning worker only. 1 parks ordinary work; 0 permits it. Executes at most
 * once, outside the gate. READY continues parking until every peer releases,
 * then finish runs once outside the gate before reusing the slot. New joins are
 * refused while finishing. Closed queues skip finish: no restart on owner stop.
 * Gate contention conservatively parks; no caller spins or sleeps here. */
int vco_close_queue_pump(VcoCloseQueue *, VcoCloseExecute, VcoCloseFinish, void *context);
#endif
