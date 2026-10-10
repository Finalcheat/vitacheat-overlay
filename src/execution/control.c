#include "control.h"
#include <string.h>
typedef char request_size_check[sizeof(VcoControlRequest) == 56 ? 1 : -1];
typedef char row_size_check[sizeof(VcoControlRow) == 32 ? 1 : -1];
typedef char reply_size_check[sizeof(VcoControlReply) == 568 ? 1 : -1];
static VcoControlStatus reject(VcoControlReply *reply, VcoControlStatus status)
{
    if (reply)
        reply->status = status;
    return status;
}
static void snapshot(const VcoEngine *e, unsigned first, unsigned count, VcoControlReply *reply)
{
    reply->session = e->token;
    reply->revision = e->revision;
    reply->first = first;
    reply->count = count;
    reply->total = e->count;
    if (vco_session_matches(e->session, e->token, true))
        reply->flags |= VCO_CONTROL_FOREGROUND;
    if (e->blocked_restore)
        reply->flags |= VCO_CONTROL_BLOCKED;
    for (unsigned i = 0; i < count; ++i) {
        const VcoExecutionRow *source = &e->rows[first + i];
        reply->rows[i] = (VcoControlRow){source->acknowledged,    source->pending,
                                         (uint32_t)source->state, (uint32_t)source->diagnostic.code,
                                         source->diagnostic.line, source->desired ? 1u : 0u};
    }
}
VcoControlStatus vco_control_dispatch(VcoEngine *e, int32_t owner, int32_t caller,
                                      const void *packet, size_t length, VcoControlReply *reply)
{
    if (!reply)
        return VCO_CONTROL_PACKET;
    /* Copy before zeroing reply, including when input and output overlap. */
    VcoControlRequest request = {0};
    bool copied = packet && length == sizeof(request);
    if (copied)
        memcpy(&request, packet, sizeof(request));
    memset(reply, 0, sizeof(*reply));
    reply->abi = VCO_CONTROL_ABI;
    reply->size = sizeof(*reply);
    if (copied)
        reply->request = request.request;
    if (owner <= 0 || caller != owner)
        return reject(reply, VCO_CONTROL_OWNER);
    if (!copied || request.abi != VCO_CONTROL_ABI || request.size != sizeof(request) ||
        !request.request || !request.session || !request.revision || request.reserved[0] ||
        request.reserved[1])
        return reject(reply, VCO_CONTROL_PACKET);
    reply->request = request.request;
    if (request.operation == VCO_CONTROL_SET) {
        if (request.enabled > 1 || request.count != 1)
            return reject(reply, VCO_CONTROL_PACKET);
    } else if (request.operation == VCO_CONTROL_QUERY) {
        if (request.enabled || !request.count || request.count > VCO_CONTROL_PAGE)
            return reject(reply, VCO_CONTROL_PACKET);
    } else
        return reject(reply, VCO_CONTROL_PACKET);
    if (!e || !e->bound || !vco_session_matches(e->session, e->token, false))
        return reject(reply, VCO_CONTROL_UNBOUND);
    if (request.session != e->token || request.revision != e->revision)
        return reject(reply, VCO_CONTROL_STALE);
    if (request.entry > e->count || request.count > e->count - request.entry)
        return reject(reply, VCO_CONTROL_PACKET);
    VcoError error = VCO_OK;
    if (request.operation == VCO_CONTROL_SET) {
        VcoExecutionCommand command = {request.request, request.session, request.revision,
                                       request.entry, request.enabled != 0};
        error = vco_engine_submit(e, command);
    }
    snapshot(e, request.entry, request.count, reply);
    reply->error = error;
    return reject(reply, error ? VCO_CONTROL_REJECTED : VCO_CONTROL_OK);
}
