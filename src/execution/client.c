#include "client.h"
#include <string.h>
void vco_control_client_init(VcoControlClient *c)
{
    memset(c, 0, sizeof(*c));
}
bool vco_control_client_bind(VcoControlClient *c, uint64_t session, uint64_t revision,
                             unsigned count)
{
    if (!c || !session || !revision || count > VCO_ENGINE_ENTRIES)
        return false;
    uint64_t sequence = c->sequence;
    memset(c, 0, sizeof(*c));
    c->sequence = sequence;
    c->session = session;
    c->revision = revision;
    c->count = count;
    c->bound = true;
    return true;
}
static bool begin(VcoControlClient *c, unsigned operation, unsigned entry, unsigned count,
                  bool enabled, VcoControlRequest *out)
{
    if (!c || !out || !c->bound || c->waiting || c->sequence == UINT64_MAX || !count ||
        count > VCO_CONTROL_PAGE || entry > c->count || count > c->count - entry)
        return false;
    c->flight = (VcoControlRequest){
        ++c->sequence, c->session, c->revision,       VCO_CONTROL_ABI, sizeof(c->flight),
        operation,     entry,      enabled ? 1u : 0u, count,           {0, 0}};
    c->waiting = true;
    *out = c->flight;
    return true;
}
bool vco_control_client_query(VcoControlClient *c, unsigned first, unsigned count,
                              VcoControlRequest *out)
{
    return begin(c, VCO_CONTROL_QUERY, first, count, false, out);
}
bool vco_control_client_set(VcoControlClient *c, unsigned entry, bool enabled,
                            VcoControlRequest *out)
{
    if (!c || entry >= c->count || !c->known[entry] || c->rows[entry].pending)
        return false;
    return begin(c, VCO_CONTROL_SET, entry, 1, enabled, out);
}
bool vco_control_client_retry(const VcoControlClient *c, VcoControlRequest *out)
{
    if (!c || !out || !c->waiting)
        return false;
    *out = c->flight;
    return true;
}
static bool empty_rows(const VcoControlReply *reply, unsigned first)
{
    for (unsigned i = first; i < VCO_CONTROL_PAGE; ++i) {
        const VcoControlRow *r = &reply->rows[i];
        if (r->acknowledged || r->pending || r->state || r->error || r->line || r->desired)
            return false;
    }
    return true;
}
bool vco_control_client_accept(VcoControlClient *c, const void *packet, size_t length)
{
    if (!c || !c->waiting || !packet || length != sizeof(VcoControlReply))
        return false;
    VcoControlReply reply;
    memcpy(&reply, packet, sizeof(reply));
    if (reply.abi != VCO_CONTROL_ABI || reply.size != sizeof(reply) ||
        reply.request != c->flight.request || reply.status > VCO_CONTROL_REJECTED ||
        reply.error > VCO_E_METADATA)
        return false;
    bool snapshot = reply.status == VCO_CONTROL_OK || reply.status == VCO_CONTROL_REJECTED;
    if (snapshot) {
        if (reply.session != c->session || reply.revision != c->revision ||
            reply.first != c->flight.entry || reply.count != c->flight.count ||
            reply.total != c->count ||
            (reply.flags & ~(VCO_CONTROL_FOREGROUND | VCO_CONTROL_BLOCKED)) ||
            (reply.status == VCO_CONTROL_OK && reply.error) ||
            (reply.status == VCO_CONTROL_REJECTED &&
             (!reply.error || c->flight.operation != VCO_CONTROL_SET)) ||
            !empty_rows(&reply, reply.count))
            return false;
        /* Validate the entire batch before publishing any row. */
        for (unsigned i = 0; i < reply.count; ++i) {
            const VcoControlRow *r = &reply.rows[i];
            const unsigned index = reply.first + i;
            const VcoControlRow *previous = &c->rows[index];
            if (r->state > VCO_ENTRY_UNSUPPORTED || r->error > VCO_E_METADATA || r->desired > 1 ||
                ((r->state == VCO_ENTRY_OFF || r->state == VCO_ENTRY_ERROR ||
                  r->state == VCO_ENTRY_UNSUPPORTED) &&
                 (r->desired || r->pending)) ||
                ((r->state == VCO_ENTRY_ERROR || r->state == VCO_ENTRY_UNSUPPORTED) && !r->error) ||
                (r->pending && r->pending <= r->acknowledged) ||
                ((r->state == VCO_ENTRY_ON || r->state == VCO_ENTRY_PAUSED) &&
                 (!r->desired || r->pending)) ||
                ((r->state == VCO_ENTRY_ENABLING || r->state == VCO_ENTRY_DISABLING) &&
                 !r->pending) ||
                ((r->state == VCO_ENTRY_OFF || r->state == VCO_ENTRY_ON ||
                  r->state == VCO_ENTRY_PAUSED) &&
                 (r->error || r->line)) ||
                ((r->state == VCO_ENTRY_ON || r->state == VCO_ENTRY_PAUSED ||
                  r->state == VCO_ENTRY_ERROR) &&
                 !r->acknowledged) ||
                (r->state == VCO_ENTRY_UNSUPPORTED && r->acknowledged) ||
                (c->known[index] &&
                 (r->acknowledged < previous->acknowledged ||
                  (previous->pending && r->pending && r->pending != previous->pending) ||
                  (previous->pending && !r->pending && r->acknowledged < previous->pending))))
                return false;
        }
        if (c->flight.operation == VCO_CONTROL_SET && reply.status == VCO_CONTROL_OK) {
            const VcoControlRow *r = &reply.rows[0];
            if (r->pending) {
                if (r->pending != c->flight.request ||
                    r->state != (c->flight.enabled ? VCO_ENTRY_ENABLING : VCO_ENTRY_DISABLING))
                    return false;
            } else if (r->acknowledged != c->flight.request ||
                       (r->state != VCO_ENTRY_ERROR && r->desired != c->flight.enabled))
                return false;
        }
        for (unsigned i = 0; i < reply.count; ++i) {
            c->rows[reply.first + i] = reply.rows[i];
            c->known[reply.first + i] = true;
            /* A restarted UI can query an engine that already processed higher
             * IDs. Its next SET must exceed the observed per-row watermark. */
            if (reply.rows[i].acknowledged > c->sequence)
                c->sequence = reply.rows[i].acknowledged;
            if (reply.rows[i].pending > c->sequence)
                c->sequence = reply.rows[i].pending;
        }
        c->flags = reply.flags;
    } else {
        /* Rejections deliberately contain no snapshot. Garbage must not revoke
         * the current binding or turn a malformed packet into a valid failure. */
        if (reply.session || reply.revision || reply.error || reply.first || reply.count ||
            reply.total || reply.flags || !empty_rows(&reply, 0))
            return false;
        c->bound = false;
        memset(c->known, 0, sizeof(c->known));
        memset(c->rows, 0, sizeof(c->rows));
        c->flags = 0;
    }
    c->last_status = reply.status;
    c->last_error = reply.error;
    c->waiting = false;
    return true;
}
