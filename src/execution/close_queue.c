#include "close_queue.h"
#include <string.h>
#if !defined(__GCC_ATOMIC_INT_LOCK_FREE) || __GCC_ATOMIC_INT_LOCK_FREE != 2
#error "Close queue requires lock-free 32-bit integer atomics"
#endif
enum { EMPTY, QUEUED, RUNNING, READY, FINISH_PENDING, FINISH_RUNNING };
typedef char close_atomic_size_check[sizeof(unsigned int) == sizeof(uint32_t) ? 1 : -1];
static int enter(VcoCloseQueue *q)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&q->gate, &expected, 1, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}
static void leave(VcoCloseQueue *q)
{
    __atomic_store_n(&q->gate, 0, __ATOMIC_RELEASE);
}
void vco_close_queue_init(VcoCloseQueue *q)
{
    if (q)
        memset(q, 0, sizeof(*q));
}
int vco_close_queue_open(const VcoCloseQueue *q)
{
    return q && !__atomic_load_n(&q->closed, __ATOMIC_ACQUIRE);
}
void vco_close_queue_stop(VcoCloseQueue *q)
{
    if (q)
        __atomic_store_n(&q->closed, 1, __ATOMIC_RELEASE);
}
VcoCloseStatus vco_close_queue_submit(VcoCloseQueue *q, uint64_t requester,
                                      const VcoEngineCloseKey *key, uint64_t *ticket)
{
    if (!q || !requester || !ticket || !vco_close_key_valid(key))
        return VCO_CLOSE_ARGUMENT;
    if (!enter(q))
        return VCO_CLOSE_BUSY;
    VcoCloseStatus status = VCO_CLOSE_OK;
    uint32_t state = __atomic_load_n(&q->state, __ATOMIC_ACQUIRE);
    if (!vco_close_queue_open(q))
        status = VCO_CLOSE_CLOSED;
    else if (state == FINISH_PENDING || state == FINISH_RUNNING)
        status = VCO_CLOSE_BUSY;
    else if (state != EMPTY && !vco_close_key_equal(&q->key, key))
        status = VCO_CLOSE_BUSY;
    else {
        for (unsigned i = 0; i < q->used && state != EMPTY; ++i)
            if (q->peers[i].requester == requester) {
                status = q->peers[i].released ? VCO_CLOSE_TICKET : VCO_CLOSE_OK;
                if (!status)
                    *ticket = q->ticket;
                goto done;
            }
        if (state == EMPTY) {
            if (q->sequence == UINT64_MAX) {
                status = VCO_CLOSE_EXHAUSTED;
                goto done;
            }
            q->key = *key;
            q->ticket = ++q->sequence;
            q->used = q->active = 0;
        } else if (q->used == VCO_CLOSE_PEERS) {
            status = VCO_CLOSE_LIMIT;
            goto done;
        }
        q->peers[q->used++] = (VcoClosePeer){requester, false};
        ++q->active;
        *ticket = q->ticket;
        if (state == EMPTY)
            __atomic_store_n(&q->state, QUEUED, __ATOMIC_RELEASE);
    }
done:
    leave(q);
    return status;
}
static int peer(const VcoCloseQueue *q, uint64_t requester, uint64_t ticket)
{
    if (q->ticket != ticket || __atomic_load_n(&q->state, __ATOMIC_ACQUIRE) == EMPTY)
        return -1;
    for (unsigned i = 0; i < q->used; ++i)
        if (q->peers[i].requester == requester && !q->peers[i].released)
            return (int)i;
    return -1;
}
VcoCloseStatus vco_close_queue_receive(VcoCloseQueue *q, uint64_t requester, uint64_t ticket,
                                       VcoCloseReply *reply)
{
    if (!q || !requester || !ticket || !reply)
        return VCO_CLOSE_ARGUMENT;
    if (!enter(q))
        return VCO_CLOSE_BUSY;
    VcoCloseStatus result = peer(q, requester, ticket) < 0 ? VCO_CLOSE_TICKET : VCO_CLOSE_WAIT;
    if (result == VCO_CLOSE_WAIT && __atomic_load_n(&q->state, __ATOMIC_ACQUIRE) == READY) {
        *reply = q->reply;
        result = VCO_CLOSE_OK;
    }
    leave(q);
    return result;
}
static VcoCloseStatus release_peer(VcoCloseQueue *q, uint64_t requester, uint64_t ticket,
                                   bool abandoned)
{
    if (!q || !requester || !ticket)
        return VCO_CLOSE_ARGUMENT;
    if (!enter(q))
        return VCO_CLOSE_BUSY;
    int index = peer(q, requester, ticket);
    VcoCloseStatus result = VCO_CLOSE_OK;
    if (index < 0)
        result = VCO_CLOSE_TICKET;
    else if (abandoned ? vco_close_queue_open(q)
                       : __atomic_load_n(&q->state, __ATOMIC_ACQUIRE) != READY)
        result = VCO_CLOSE_WAIT;
    else {
        q->peers[index].released = true;
        if (!--q->active)
            __atomic_store_n(&q->state, abandoned ? EMPTY : FINISH_PENDING, __ATOMIC_RELEASE);
    }
    leave(q);
    return result;
}
VcoCloseStatus vco_close_queue_release(VcoCloseQueue *q, uint64_t requester, uint64_t ticket)
{
    return release_peer(q, requester, ticket, false);
}
VcoCloseStatus vco_close_queue_abandon_after_stop(VcoCloseQueue *q, uint64_t requester,
                                                  uint64_t ticket)
{
    return release_peer(q, requester, ticket, true);
}
int vco_close_queue_pump(VcoCloseQueue *q, VcoCloseExecute execute, VcoCloseFinish finish,
                         void *context)
{
    if (!q)
        return 0;
    if (!enter(q))
        return 1;
    uint32_t state = __atomic_load_n(&q->state, __ATOMIC_ACQUIRE);
    if (state == FINISH_PENDING) {
        VcoCloseReply reply = q->reply;
        __atomic_store_n(&q->state, FINISH_RUNNING, __ATOMIC_RELEASE);
        leave(q);
        if (finish && vco_close_queue_open(q))
            finish(context, &reply);
        /* Every peer is already ended, and FINISH_RUNNING prohibits new
         * producers from borrowing/replacing this key while metadata changes. */
        __atomic_store_n(&q->state, EMPTY, __ATOMIC_RELEASE);
        return 0;
    }
    if (state != QUEUED || !execute) {
        leave(q);
        return state != EMPTY;
    }
    VcoCloseReply reply = {.ticket = q->ticket, .key = q->key};
    __atomic_store_n(&q->state, RUNNING, __ATOMIC_RELEASE);
    leave(q);
    reply.error = execute(context, &reply.key, &reply.cleanup);
    /* RUNNING forbids ordinary release/read/reset. No gate is needed to write
     * the owner-only result, and no caller can recycle this request meanwhile. */
    q->reply = reply;
    __atomic_store_n(&q->state, READY, __ATOMIC_RELEASE);
    return 1;
}
