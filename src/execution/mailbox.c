#include "mailbox.h"
#include <string.h>
enum { EMPTY, QUEUED, RUNNING, READY };
static bool enter(VcoControlMailbox *m)
{
    return __atomic_exchange_n(&m->gate, 1, __ATOMIC_ACQUIRE) == 0;
}
static void leave(VcoControlMailbox *m)
{
    __atomic_store_n(&m->gate, 0, __ATOMIC_RELEASE);
}
static bool closing(const VcoControlMailbox *m)
{
    return __atomic_load_n(&m->closing, __ATOMIC_ACQUIRE) != 0;
}
static VcoMailboxStatus authorized(const VcoControlMailbox *m, int32_t caller)
{
    if (!m)
        return VCO_MAILBOX_ARGUMENT;
    if (m->owner <= 0 || caller != m->owner)
        return VCO_MAILBOX_OWNER;
    return closing(m) ? VCO_MAILBOX_CLOSED : VCO_MAILBOX_OK;
}
void vco_mailbox_init(VcoControlMailbox *m, VcoEngine *e, int32_t owner)
{
    memset(m, 0, sizeof(*m));
    m->engine = e;
    m->owner = owner;
}
VcoMailboxStatus vco_mailbox_submit(VcoControlMailbox *m, int32_t caller, const void *packet,
                                    size_t length, uint64_t *ticket)
{
    VcoMailboxStatus result = authorized(m, caller);
    if (result)
        return result;
    if (!packet || !ticket || length != sizeof(VcoControlRequest))
        return VCO_MAILBOX_ARGUMENT;
    /* The transport must supply kernel-owned buffers, not user pointers. */
    VcoControlRequest copy;
    memcpy(&copy, packet, sizeof(copy));
    if (!enter(m))
        return VCO_MAILBOX_BUSY;
    uint32_t state = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
    if (closing(m))
        result = VCO_MAILBOX_CLOSED;
    else if (state != EMPTY) {
        if (!memcmp(&copy, &m->request, sizeof(copy))) {
            *ticket = m->ticket;
            result = VCO_MAILBOX_OK;
        } else
            result = VCO_MAILBOX_BUSY;
    } else if (m->sequence == UINT64_MAX)
        result = VCO_MAILBOX_EXHAUSTED;
    else {
        m->request = copy;
        m->ticket = ++m->sequence;
        *ticket = m->ticket;
        __atomic_store_n(&m->state, QUEUED, __ATOMIC_RELEASE);
        result = VCO_MAILBOX_OK;
    }
    leave(m);
    return result;
}
VcoMailboxStatus vco_mailbox_resume(VcoControlMailbox *m, int32_t caller,
                                    VcoControlRequest *request, uint64_t *ticket)
{
    VcoMailboxStatus result = authorized(m, caller);
    if (result)
        return result;
    if (!request || !ticket)
        return VCO_MAILBOX_ARGUMENT;
    if (!enter(m))
        return VCO_MAILBOX_BUSY;
    if (closing(m))
        result = VCO_MAILBOX_CLOSED;
    else if (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == EMPTY)
        result = VCO_MAILBOX_EMPTY;
    else {
        *request = m->request;
        *ticket = m->ticket;
        result = VCO_MAILBOX_OK;
    }
    leave(m);
    return result;
}
VcoMailboxStatus vco_mailbox_receive(VcoControlMailbox *m, int32_t caller, uint64_t ticket,
                                     VcoControlReply *out)
{
    VcoMailboxStatus result = authorized(m, caller);
    if (result)
        return result;
    if (!ticket || !out)
        return VCO_MAILBOX_ARGUMENT;
    if (!enter(m))
        return VCO_MAILBOX_BUSY;
    uint32_t state = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
    if (closing(m))
        result = VCO_MAILBOX_CLOSED;
    else if (ticket != m->ticket || state == EMPTY)
        result = VCO_MAILBOX_TICKET;
    else if (state != READY)
        result = VCO_MAILBOX_WAIT;
    else {
        *out = m->reply;
        result = VCO_MAILBOX_OK;
    }
    leave(m);
    return result;
}
VcoMailboxStatus vco_mailbox_ack(VcoControlMailbox *m, int32_t caller, uint64_t ticket)
{
    VcoMailboxStatus result = authorized(m, caller);
    if (result)
        return result;
    if (!ticket)
        return VCO_MAILBOX_ARGUMENT;
    if (!enter(m))
        return VCO_MAILBOX_BUSY;
    uint32_t state = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
    if (closing(m))
        result = VCO_MAILBOX_CLOSED;
    else if (ticket != m->ticket || state == EMPTY)
        result = VCO_MAILBOX_TICKET;
    else if (state != READY)
        result = VCO_MAILBOX_WAIT;
    else {
        __atomic_store_n(&m->state, EMPTY, __ATOMIC_RELEASE);
        result = VCO_MAILBOX_OK;
    }
    leave(m);
    return result;
}
int vco_mailbox_pump(VcoControlMailbox *m, uint64_t now_us)
{
    if (!m || __atomic_load_n(&m->stopped, __ATOMIC_ACQUIRE))
        return 0;
    if (!enter(m))
        return 1;
    if (closing(m)) {
        /* No new clients can enter. Cleanup runs outside the short-copy gate. */
        leave(m);
        m->stop_error = m->engine ? vco_engine_unbind(m->engine) : VCO_OK;
        __atomic_store_n(&m->stopped, 1, __ATOMIC_RELEASE);
        return 0;
    }
    bool work = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == QUEUED;
    VcoControlRequest request;
    if (work) {
        request = m->request;
        __atomic_store_n(&m->state, RUNNING, __ATOMIC_RELEASE);
    }
    leave(m);
    bool stepped = false;
    if (work) {
        /* Reflect lifecycle changes before answering a query. SET must instead
         * enter the command queue before the next tick, so OFF wins over writes. */
        if (request.operation == VCO_CONTROL_QUERY && !closing(m)) {
            vco_engine_step(m->engine, now_us);
            stepped = true;
        }
        VcoControlReply reply;
        vco_control_dispatch(m->engine, m->owner, m->owner, &request, sizeof(request), &reply);
        /* RUNNING prevents clients from reading/acknowledging this storage;
         * publish only after all bytes are ready. Duplicate submit reads request,
         * not reply, so can proceed concurrently. */
        m->reply = reply;
        __atomic_store_n(&m->state, READY, __ATOMIC_RELEASE);
    }
    if (!stepped && !closing(m))
        vco_engine_step(m->engine, now_us);
    return 1;
}
int vco_mailbox_open(const VcoControlMailbox *m)
{
    return m && m->engine && m->owner > 0 && !closing(m);
}
void vco_mailbox_stop(VcoControlMailbox *m)
{
    if (m)
        __atomic_store_n(&m->closing, 1, __ATOMIC_RELEASE);
}
int vco_mailbox_stop_result(const VcoControlMailbox *m, VcoError *out)
{
    if (!m || !out || !__atomic_load_n(&m->stopped, __ATOMIC_ACQUIRE))
        return 0;
    *out = m->stop_error;
    return 1;
}
