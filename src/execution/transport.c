#include "transport.h"
#include <string.h>

static void invalidate(VcoControlTransport *t)
{
    uint64_t sequence = t->client.sequence;
    if (t->client.bound)
        vco_control_client_bind(&t->client, t->client.session, t->client.revision, t->client.count);
    else {
        vco_control_client_init(&t->client);
        t->client.sequence = sequence;
    }
    t->current = false;
}
static void idle(VcoControlTransport *t)
{
    t->phase = VCO_TRANSPORT_IDLE;
    t->ticket = 0;
    t->current = false;
    t->invalid_reply = false;
    memset(&t->request, 0, sizeof(t->request));
}
static bool valid_request(const VcoControlRequest *r)
{
    return r->abi == VCO_CONTROL_ABI && r->size == sizeof(*r) && r->request && r->session &&
           r->revision && !r->reserved[0] && !r->reserved[1] && r->count &&
           r->count <= VCO_CONTROL_PAGE &&
           ((r->operation == VCO_CONTROL_QUERY && !r->enabled) ||
            (r->operation == VCO_CONTROL_SET && r->count == 1 && r->enabled <= 1));
}
static VcoMailboxStatus invalid(VcoControlTransport *t)
{
    t->invalid_reply = true;
    t->last_status = VCO_MAILBOX_ARGUMENT;
    return t->last_status;
}
bool vco_transport_init(VcoControlTransport *t, VcoTransportOps ops, void *context)
{
    if (!t)
        return false;
    memset(t, 0, sizeof(*t));
    if (!ops.submit || !ops.resume || !ops.receive || !ops.ack) {
        t->last_status = VCO_MAILBOX_ARGUMENT;
        return false;
    }
    t->ops = ops;
    t->context = context;
    t->phase = VCO_TRANSPORT_RESUME;
    return true;
}
bool vco_transport_bind(VcoControlTransport *t, uint64_t session, uint64_t revision, unsigned count)
{
    if (!t || t->phase == VCO_TRANSPORT_DISABLED ||
        !vco_control_client_bind(&t->client, session, revision, count))
        return false;
    t->current = false;
    /* SUBMIT can mean either not sent yet or accepted with a lost output copy.
     * Discover which, instead of sending an old command after the rebind. */
    if (t->phase == VCO_TRANSPORT_SUBMIT)
        t->phase = VCO_TRANSPORT_RESUME;
    return true;
}
void vco_transport_reconnect(VcoControlTransport *t)
{
    if (!t || t->phase == VCO_TRANSPORT_DISABLED)
        return;
    invalidate(t);
    t->phase = VCO_TRANSPORT_RESUME;
    t->invalid_reply = false;
}
bool vco_transport_idle(const VcoControlTransport *t)
{
    return t && t->phase == VCO_TRANSPORT_IDLE;
}
bool vco_transport_query(VcoControlTransport *t, unsigned first, unsigned count)
{
    if (!vco_transport_idle(t) || !vco_control_client_query(&t->client, first, count, &t->request))
        return false;
    t->current = true;
    t->phase = VCO_TRANSPORT_SUBMIT;
    return true;
}
bool vco_transport_set(VcoControlTransport *t, unsigned entry, bool enabled)
{
    if (!vco_transport_idle(t) || !vco_control_client_set(&t->client, entry, enabled, &t->request))
        return false;
    t->current = true;
    t->phase = VCO_TRANSPORT_SUBMIT;
    return true;
}
static bool accept_recovery(VcoControlTransport *t, const VcoControlReply *reply)
{
    const VcoControlRequest *r = &t->request;
    if (!valid_request(r)) {
        /* The mailbox can retain a malformed request from an older UI. Only
         * its exact, canonical PACKET rejection is safe to drain. It cannot
         * have reached the engine, so it contributes no sequence watermark.
         * A zero request ID is matched by the transport ticket in this case. */
        VcoControlReply rejected = {0};
        rejected.request = r->request;
        rejected.abi = VCO_CONTROL_ABI;
        rejected.size = sizeof(rejected);
        rejected.status = VCO_CONTROL_PACKET;
        return !memcmp(reply, &rejected, sizeof(rejected));
    }
    if (reply->total > VCO_ENGINE_ENTRIES)
        return false;
    bool snapshot = reply->status == VCO_CONTROL_OK || reply->status == VCO_CONTROL_REJECTED;
    if (snapshot && (r->entry > reply->total || r->count > reply->total - r->entry))
        return false;
    vco_control_client_bind(&t->recovery, r->session, r->revision, reply->total);
    t->recovery.flight = *r;
    t->recovery.waiting = true;
    if (!vco_control_client_accept(&t->recovery, reply, sizeof(*reply)))
        return false;
    /* Preserve only the sequence watermark, never the old binding or rows. */
    if (t->client.sequence < t->recovery.sequence)
        t->client.sequence = t->recovery.sequence;
    return true;
}
VcoMailboxStatus vco_transport_tick(VcoControlTransport *t)
{
    if (!t || t->phase == VCO_TRANSPORT_DISABLED)
        return VCO_MAILBOX_ARGUMENT;
    if (t->phase == VCO_TRANSPORT_IDLE) {
        t->last_status = VCO_MAILBOX_OK;
        return t->last_status;
    }
    if (t->phase == VCO_TRANSPORT_RESUME) {
        VcoControlRequest request = {0};
        uint64_t ticket = 0;
        t->last_status = t->ops.resume(t->context, &request, &ticket);
        if (t->last_status == VCO_MAILBOX_EMPTY) {
            if (t->client.waiting)
                invalidate(t);
            idle(t);
        } else if (t->last_status == VCO_MAILBOX_OK) {
            if (!ticket)
                return invalid(t);
            if (!t->current || !t->client.waiting ||
                memcmp(&request, &t->client.flight, sizeof(request)))
                invalidate(t);
            t->request = request;
            t->ticket = ticket;
            if (valid_request(&request) && t->client.sequence < request.request)
                t->client.sequence = request.request;
            t->invalid_reply = false;
            t->phase = VCO_TRANSPORT_RECEIVE;
        }
    } else if (t->phase == VCO_TRANSPORT_SUBMIT) {
        uint64_t ticket = 0;
        t->last_status = t->ops.submit(t->context, &t->request, &ticket);
        if (t->last_status == VCO_MAILBOX_OK) {
            if (!ticket)
                return invalid(t);
            t->ticket = ticket;
            t->invalid_reply = false;
            t->phase = VCO_TRANSPORT_RECEIVE;
        }
    } else if (t->phase == VCO_TRANSPORT_RECEIVE) {
        VcoControlReply reply = {0};
        t->last_status = t->ops.receive(t->context, t->ticket, &reply);
        if (t->last_status == VCO_MAILBOX_OK) {
            bool accepted = t->current
                                ? vco_control_client_accept(&t->client, &reply, sizeof(reply))
                                : accept_recovery(t, &reply);
            if (!accepted)
                return invalid(t);
            t->invalid_reply = false;
            t->phase = VCO_TRANSPORT_ACK;
        } else if (t->last_status == VCO_MAILBOX_TICKET)
            t->phase = VCO_TRANSPORT_RESUME;
    } else if (t->phase == VCO_TRANSPORT_ACK) {
        t->last_status = t->ops.ack(t->context, t->ticket);
        if (t->last_status == VCO_MAILBOX_OK)
            idle(t);
        else if (t->last_status == VCO_MAILBOX_TICKET)
            t->phase = VCO_TRANSPORT_RESUME;
    } else
        return invalid(t);
    return t->last_status;
}
