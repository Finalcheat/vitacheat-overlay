#include "broker.h"
#include <string.h>
enum { EMPTY, PENDING, DONE };
typedef char binding_size_check[sizeof(VcoAgentBinding) == 40 ? 1 : -1];
typedef char message_size_check[sizeof(VcoAgentMessage) == 40 ? 1 : -1];
typedef char result_size_check[sizeof(VcoAgentResult) == 680 ? 1 : -1];

static VcoAgentStatus shell(const VcoAgentBroker *b, int32_t caller)
{
    return !b ? VCO_AGENT_ARGUMENT : caller != b->owner ? VCO_AGENT_OWNER : VCO_AGENT_OK;
}
static VcoAgentStatus game(const VcoAgentBroker *b, int32_t caller, uint64_t session)
{
    if (!b)
        return VCO_AGENT_ARGUMENT;
    if (!session || !b->binding.session || session != b->binding.session)
        return VCO_AGENT_STALE;
    return caller == b->binding.pid ? VCO_AGENT_OK : VCO_AGENT_OWNER;
}
static void empty_slot(VcoAgentBroker *b)
{
    b->phase = EMPTY;
    memset(&b->message, 0, sizeof(b->message));
    memset(&b->result, 0, sizeof(b->result));
    memset(&b->control, 0, sizeof(b->control));
}
static void empty_status(VcoAgentBroker *b)
{
    b->status = (VcoServiceStatus){
        .abi = VCO_SERVICE_ABI, .size = sizeof(b->status), .state = VCO_SERVICE_NO_GAME};
}
static void reset_modules(VcoAgentBroker *b)
{
    b->module_ticket = 0;
    memset(b->module_claims, 0, sizeof(b->module_claims));
    b->module_pending = b->module_ready = b->module_clear = false;
    b->module_error = VCO_OK;
}
bool vco_broker_module_busy(const VcoAgentBroker *b)
{
    if (!b)
        return false;
    if (b->module_pending)
        return true;
    for (unsigned i = 0; i < VCO_MODULE_CALLS; ++i)
        if (b->module_claims[i])
            return true;
    return false;
}
VcoAgentStatus vco_broker_init(VcoAgentBroker *b, int32_t owner, void *upload, size_t capacity)
{
    if (!b || owner <= 0 || !upload || capacity < VCO_UPLOAD_MAX)
        return VCO_AGENT_ARGUMENT;
    memset(b, 0, sizeof(*b));
    b->owner = owner;
    b->upload = upload;
    b->upload_capacity = capacity;
    empty_status(b);
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_open(VcoAgentBroker *b, int32_t pid, const char title[10],
                               VcoAgentBinding *out)
{
    VcoIdentity identity = {.pid = pid, .instance = 1};
    if (!b || !out || !title)
        return VCO_AGENT_ARGUMENT;
    memcpy(identity.title_id, title, sizeof(identity.title_id));
    if (!vco_identity_valid(&identity))
        return VCO_AGENT_ARGUMENT;
    if (b->next_session == UINT64_MAX)
        return VCO_AGENT_LIMIT;
    empty_slot(b);
    empty_status(b);
    b->attached = b->runnable = b->quiescent = false;
    reset_modules(b);
    b->binding = (VcoAgentBinding){
        .session = ++b->next_session, .abi = VCO_AGENT_ABI, .size = sizeof(b->binding), .pid = pid};
    memcpy(b->binding.title_id, title, 10);
    b->status.session = b->binding.session;
    b->status.pid = pid;
    memcpy(b->status.title_id, title, 10);
    b->status.state = VCO_SERVICE_STARTING;
    *out = b->binding;
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_end(VcoAgentBroker *b, uint64_t session)
{
    if (!b)
        return VCO_AGENT_ARGUMENT;
    if (!session || session != b->binding.session)
        return VCO_AGENT_STALE;
    empty_slot(b);
    empty_status(b);
    memset(&b->binding, 0, sizeof(b->binding));
    b->attached = b->runnable = b->quiescent = false;
    reset_modules(b);
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_observe(VcoAgentBroker *b, uint64_t session, bool runnable)
{
    if (!b)
        return VCO_AGENT_ARGUMENT;
    if (!session || session != b->binding.session)
        return VCO_AGENT_STALE;
    b->runnable = runnable;
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_attach(VcoAgentBroker *b, int32_t caller, const VcoAgentBinding *binding)
{
    if (!binding)
        return VCO_AGENT_ARGUMENT;
    VcoAgentStatus error = game(b, caller, binding->session);
    if (error)
        return error;
    if (memcmp(binding, &b->binding, sizeof(*binding)))
        return VCO_AGENT_ARGUMENT;
    if (b->quiescent)
        return VCO_AGENT_CLOSED;
    if (!b->attached) {
        b->attached = true;
        b->status.state = VCO_SERVICE_BACKGROUND;
    }
    return VCO_AGENT_OK;
}
static VcoAgentStatus admit(VcoAgentBroker *b, int32_t caller)
{
    VcoAgentStatus error = shell(b, caller);
    if (error)
        return error;
    return b->attached ? VCO_AGENT_OK : VCO_AGENT_STALE;
}
static VcoAgentStatus enqueue(VcoAgentBroker *b, unsigned operation, size_t length,
                              uint64_t *ticket)
{
    if (b->next_ticket == UINT64_MAX)
        return VCO_AGENT_LIMIT;
    b->message = (VcoAgentMessage){.session = b->binding.session,
                                   .ticket = ++b->next_ticket,
                                   .abi = VCO_AGENT_ABI,
                                   .size = sizeof(b->message),
                                   .operation = operation,
                                   .length = (uint32_t)length};
    b->phase = PENDING;
    *ticket = b->message.ticket;
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_rebind_owner(VcoAgentBroker *b, int32_t owner)
{
    if (!b || owner <= 0)
        return VCO_AGENT_ARGUMENT;
    b->owner = owner;
    b->runnable = false;
    if (b->quiescent)
        return VCO_AGENT_OK; /* Keep the immutable STOPPED retry receipt. */
    if (vco_broker_module_busy(b)) {
        /* A Shell restart cannot interrupt a joined SDK operation. Clear the
         * retained list only after the module boundary has finished. */
        b->module_clear = true;
        return VCO_AGENT_OK;
    }
    empty_slot(b);
    if (b->attached) {
        uint64_t ticket;
        VcoAgentStatus error = enqueue(b, VCO_AGENT_CLEAR, 0, &ticket);
        b->status.state = error ? VCO_SERVICE_ERROR : VCO_SERVICE_LOADING;
        b->status.error = error ? VCO_E_LIMIT : VCO_OK;
        return error;
    }
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_load(VcoAgentBroker *b, int32_t caller, const void *packet, size_t length,
                               uint64_t *ticket)
{
    VcoAgentStatus error = admit(b, caller);
    if (error)
        return error;
    if (vco_broker_module_busy(b))
        return VCO_AGENT_BUSY;
    if (!ticket || !packet || length < sizeof(VcoUploadHeader) || length > b->upload_capacity ||
        length > VCO_UPLOAD_MAX)
        return VCO_AGENT_ARGUMENT;
    unsigned operation = VCO_AGENT_LOAD;
    if (length) {
        VcoUploadHeader h;
        if (length < sizeof(h))
            return VCO_AGENT_ARGUMENT;
        memcpy(&h, packet, sizeof(h));
        if (h.abi != VCO_UPLOAD_ABI || h.size != length || !h.request || !h.content_key ||
            h.target_session != b->binding.session || h.pid != (uint32_t)b->binding.pid ||
            memcmp(h.title_id, b->binding.title_id, 10) ||
            length != vco_upload_size(h.entries, h.codes))
            return VCO_AGENT_STALE;
    }
    if (b->phase != EMPTY) {
        if (b->message.operation != operation || b->message.length != length ||
            (length && memcmp(b->upload, packet, length)))
            return VCO_AGENT_BUSY;
        *ticket = b->message.ticket;
        return VCO_AGENT_OK;
    }
    if (b->next_ticket == UINT64_MAX)
        return VCO_AGENT_LIMIT;
    if (length)
        memcpy(b->upload, packet, length);
    return enqueue(b, operation, length, ticket);
}
VcoAgentStatus vco_broker_clear(VcoAgentBroker *b, int32_t caller, uint64_t session,
                                uint64_t *ticket)
{
    VcoAgentStatus error = admit(b, caller);
    if (error)
        return error;
    if (vco_broker_module_busy(b))
        return VCO_AGENT_BUSY;
    if (!ticket)
        return VCO_AGENT_ARGUMENT;
    if (!session || session != b->binding.session)
        return VCO_AGENT_STALE;
    if (b->phase != EMPTY) {
        if (b->message.operation != VCO_AGENT_CLEAR)
            return VCO_AGENT_BUSY;
        *ticket = b->message.ticket;
        return VCO_AGENT_OK;
    }
    return enqueue(b, VCO_AGENT_CLEAR, 0, ticket);
}
VcoAgentStatus vco_broker_submit(VcoAgentBroker *b, int32_t caller, const VcoControlRequest *input,
                                 uint64_t *ticket)
{
    VcoAgentStatus error = admit(b, caller);
    if (error)
        return error;
    if (vco_broker_module_busy(b))
        return VCO_AGENT_BUSY;
    if (!input || !ticket)
        return VCO_AGENT_ARGUMENT;
    VcoControlRequest request;
    memcpy(&request, input, sizeof(request));
    if (request.abi != VCO_CONTROL_ABI || request.size != sizeof(request) || !request.request ||
        request.session != b->binding.session || !request.revision || request.reserved[0] ||
        request.reserved[1])
        return VCO_AGENT_ARGUMENT;
    if (b->phase != EMPTY) {
        if (b->message.operation != VCO_AGENT_CONTROL ||
            memcmp(&b->control, &request, sizeof(request)))
            return VCO_AGENT_BUSY;
        *ticket = b->message.ticket;
        return VCO_AGENT_OK;
    }
    if (b->next_ticket == UINT64_MAX)
        return VCO_AGENT_LIMIT;
    b->control = request;
    return enqueue(b, VCO_AGENT_CONTROL, sizeof(request), ticket);
}
VcoAgentStatus vco_broker_poll(VcoAgentBroker *b, int32_t caller, uint64_t session,
                               VcoAgentMessage *out, void *payload, size_t capacity)
{
    VcoAgentStatus error = game(b, caller, session);
    if (error)
        return error;
    if (!out || !b->attached)
        return VCO_AGENT_ARGUMENT;
    if (b->phase != PENDING) {
        *out = (VcoAgentMessage){.session = session,
                                 .abi = VCO_AGENT_ABI,
                                 .size = sizeof(*out),
                                 .runnable = b->runnable && !vco_broker_module_busy(b)};
        return VCO_AGENT_OK;
    }
    if (b->message.length && (!payload || capacity < b->message.length))
        return VCO_AGENT_LIMIT;
    if (b->message.operation == VCO_AGENT_LOAD)
        memcpy(payload, b->upload, b->message.length);
    else if (b->message.operation == VCO_AGENT_CONTROL)
        memcpy(payload, &b->control, sizeof(b->control));
    *out = b->message;
    out->runnable = b->runnable && !vco_broker_module_busy(b);
    return VCO_AGENT_OK;
}
static bool valid_status(const VcoAgentBroker *b, const VcoServiceStatus *s)
{
    return s->abi == VCO_SERVICE_ABI && s->size == sizeof(*s) && s->session == b->binding.session &&
           s->pid == b->binding.pid && !memcmp(s->title_id, b->binding.title_id, 10) &&
           !s->reserved && !s->padding[0] && !s->padding[1] && s->count <= VCO_ENGINE_ENTRIES &&
           (s->state == VCO_SERVICE_READY || s->state == VCO_SERVICE_BACKGROUND ||
            s->state == VCO_SERVICE_ERROR) &&
           s->error <= VCO_E_METADATA &&
           (s->revision ? s->content_key && s->catalog_revision
                        : !s->count && !s->content_key && !s->catalog_revision);
}
static bool valid_control(const VcoAgentBroker *b, const VcoControlReply *r)
{
    if (r->abi != VCO_CONTROL_ABI || r->size != sizeof(*r) || r->request != b->control.request ||
        r->status > VCO_CONTROL_REJECTED || r->error > VCO_E_METADATA ||
        r->count > VCO_CONTROL_PAGE || r->total > VCO_ENGINE_ENTRIES || r->first > r->total ||
        r->count > r->total - r->first ||
        (r->flags & ~(VCO_CONTROL_FOREGROUND | VCO_CONTROL_BLOCKED)))
        return false;
    if (r->status == VCO_CONTROL_OK &&
        (r->session != b->binding.session || r->revision != b->control.revision ||
         r->first != b->control.entry || r->count != b->control.count))
        return false;
    for (unsigned i = 0; i < r->count; ++i)
        if (r->rows[i].state > VCO_ENTRY_UNSUPPORTED || r->rows[i].error > VCO_E_METADATA ||
            r->rows[i].desired > 1)
            return false;
    return true;
}
VcoAgentStatus vco_broker_publish(VcoAgentBroker *b, int32_t caller, const VcoAgentResult *input)
{
    if (!input)
        return VCO_AGENT_ARGUMENT;
    VcoAgentResult result;
    memcpy(&result, input, sizeof(result));
    VcoAgentStatus error = game(b, caller, result.session);
    if (error)
        return error;
    if (result.abi != VCO_AGENT_ABI || result.size != sizeof(result) ||
        result.error > VCO_E_METADATA || result.error != result.status.error ||
        !valid_status(b, &result.status))
        return VCO_AGENT_ARGUMENT;
    if (result.operation == VCO_AGENT_STOPPED) {
        if (result.ticket || result.error || result.status.state != VCO_SERVICE_BACKGROUND ||
            result.status.revision || result.status.count || result.status.content_key ||
            result.status.catalog_revision)
            return VCO_AGENT_ARGUMENT;
        const uint8_t *bytes = (const uint8_t *)&result.control;
        for (unsigned i = 0; i < sizeof(result.control); ++i)
            if (bytes[i])
                return VCO_AGENT_ARGUMENT;
        if (b->quiescent)
            return memcmp(&result, &b->result, sizeof(result)) ? VCO_AGENT_ARGUMENT : VCO_AGENT_OK;
        if (!b->attached && b->status.state != VCO_SERVICE_ERROR)
            return VCO_AGENT_CLOSED;
        /* The game worker attests only after engine/native cleanup succeeds.
         * Cancel intent, preserve live SDK-call claims, and acknowledge any
         * racing module pause using that same all-OFF completion. */
        VcoError previous =
            b->status.state == VCO_SERVICE_ERROR ? (VcoError)b->status.error : VCO_OK;
        empty_slot(b);
        b->result = result;
        b->attached = b->runnable = false;
        b->quiescent = true;
        b->module_pending = b->module_clear = false;
        b->module_ready = true;
        b->module_error = VCO_OK;
        b->status = result.status;
        b->status.state = VCO_SERVICE_ERROR;
        b->status.error = previous ? previous : VCO_E_SESSION;
        return VCO_AGENT_OK;
    }
    if (b->phase == EMPTY || result.ticket != b->message.ticket ||
        result.operation != b->message.operation)
        return VCO_AGENT_STALE;
    if (b->phase == DONE)
        return memcmp(&result, &b->result, sizeof(result)) ? VCO_AGENT_ARGUMENT : VCO_AGENT_OK;
    if (result.operation == VCO_AGENT_CONTROL && !valid_control(b, &result.control))
        return VCO_AGENT_ARGUMENT;
    b->result = result;
    b->status = result.status;
    b->phase = DONE;
    if (result.operation == VCO_AGENT_MODULE_PAUSE) {
        b->module_pending = false;
        b->module_ready = true;
        b->module_error = (VcoError)result.error;
    }
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_receive(VcoAgentBroker *b, int32_t caller, uint64_t ticket,
                                  VcoAgentResult *out)
{
    VcoAgentStatus error = shell(b, caller);
    if (error)
        return error;
    if (!out || !ticket)
        return VCO_AGENT_ARGUMENT;
    if (b->phase == EMPTY || ticket != b->message.ticket)
        return VCO_AGENT_STALE;
    if (b->phase != DONE)
        return VCO_AGENT_EMPTY;
    *out = b->result;
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_ack(VcoAgentBroker *b, int32_t caller, uint64_t ticket)
{
    VcoAgentStatus error = shell(b, caller);
    if (error)
        return error;
    if (!ticket || b->phase == EMPTY || ticket != b->message.ticket)
        return VCO_AGENT_STALE;
    if (b->phase != DONE)
        return VCO_AGENT_BUSY;
    empty_slot(b);
    if (!vco_broker_module_busy(b) && b->module_clear) {
        b->module_clear = false;
        VcoAgentStatus queued = enqueue(b, VCO_AGENT_CLEAR, 0, &ticket);
        b->status.state = queued ? VCO_SERVICE_ERROR : VCO_SERVICE_LOADING;
        b->status.error = queued ? VCO_E_LIMIT : VCO_OK;
        return queued;
    }
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_resume(VcoAgentBroker *b, int32_t caller, VcoControlRequest *out,
                                 uint64_t *ticket)
{
    VcoAgentStatus error = shell(b, caller);
    if (error)
        return error;
    if (!out || !ticket)
        return VCO_AGENT_ARGUMENT;
    if (b->phase == EMPTY || b->message.operation != VCO_AGENT_CONTROL)
        return VCO_AGENT_EMPTY;
    *out = b->control;
    *ticket = b->message.ticket;
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_status(VcoAgentBroker *b, int32_t caller, VcoServiceStatus *out)
{
    VcoAgentStatus error = shell(b, caller);
    if (error)
        return error;
    if (!out)
        return VCO_AGENT_ARGUMENT;
    *out = b->status;
    if (b->attached && out->state != VCO_SERVICE_ERROR)
        out->state =
            b->runnable && !vco_broker_module_busy(b) ? VCO_SERVICE_READY : VCO_SERVICE_BACKGROUND;
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_module_begin(VcoAgentBroker *b, int32_t caller, VcoModuleCall *out)
{
    if (!b || !out)
        return VCO_AGENT_ARGUMENT;
    VcoAgentStatus error = game(b, caller, b->binding.session);
    if (error)
        return error;
    if (!b->attached)
        return VCO_AGENT_CLOSED;
    unsigned free_slot = 0;
    while (free_slot < VCO_MODULE_CALLS && b->module_claims[free_slot])
        ++free_slot;
    if (free_slot == VCO_MODULE_CALLS || b->next_claim == UINT64_MAX)
        return VCO_AGENT_LIMIT;
    if (!vco_broker_module_busy(b)) {
        if (b->next_ticket == UINT64_MAX)
            return VCO_AGENT_LIMIT;
        empty_slot(b); /* Supersede queued LOAD/CLEAR/SET and any old reply. */
        error = enqueue(b, VCO_AGENT_MODULE_PAUSE, 0, &b->module_ticket);
        if (error)
            return error;
        b->module_pending = true;
        b->module_ready = false;
        b->module_error = VCO_OK;
    }
    b->module_claims[free_slot] = ++b->next_claim;
    *out = (VcoModuleCall){b->binding.session, b->module_ticket, b->next_claim};
    return VCO_AGENT_OK;
}
static VcoAgentStatus module_claim(const VcoAgentBroker *b, const VcoModuleCall *call,
                                   unsigned *slot)
{
    if (!b || !call || !slot)
        return VCO_AGENT_ARGUMENT;
    if (!call->session || call->session != b->binding.session || !call->ticket ||
        call->ticket != b->module_ticket || !call->claim)
        return VCO_AGENT_STALE;
    for (unsigned i = 0; i < VCO_MODULE_CALLS; ++i)
        if (call->claim == b->module_claims[i]) {
            *slot = i;
            return VCO_AGENT_OK;
        }
    return VCO_AGENT_STALE;
}
VcoAgentStatus vco_broker_module_ready(VcoAgentBroker *b, const VcoModuleCall *call, VcoError *out)
{
    unsigned slot;
    VcoAgentStatus error = module_claim(b, call, &slot);
    if (error || !out)
        return error ? error : VCO_AGENT_ARGUMENT;
    if (!b->module_ready)
        return VCO_AGENT_EMPTY;
    *out = b->module_error;
    return VCO_AGENT_OK;
}
VcoAgentStatus vco_broker_module_end(VcoAgentBroker *b, const VcoModuleCall *call)
{
    unsigned slot;
    VcoAgentStatus error = module_claim(b, call, &slot);
    if (error)
        return error;
    b->module_claims[slot] = 0;
    if (!vco_broker_module_busy(b) && b->module_clear) {
        /* A ready receipt must be acknowledged before a subsequent CLEAR. */
        if (b->phase == DONE)
            return vco_broker_ack(b, b->owner, b->message.ticket);
        uint64_t ticket;
        b->module_clear = false;
        error = enqueue(b, VCO_AGENT_CLEAR, 0, &ticket);
        b->status.state = error ? VCO_SERVICE_ERROR : VCO_SERVICE_LOADING;
        b->status.error = error ? VCO_E_LIMIT : VCO_OK;
    }
    return error;
}
