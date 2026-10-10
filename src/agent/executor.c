#include "executor.h"
#include <string.h>

VcoError vco_game_executor_init(VcoGameExecutor *g, const VcoAgentBinding *binding,
                                VcoAgentAllocator allocator, VcoCheatMemory memory,
                                VcoEngineCloseNative finalize, void *context)
{
    if (!g || !binding || binding->abi != VCO_AGENT_ABI || binding->size != sizeof(*binding) ||
        !allocator.allocate || !allocator.release || !memory.alive || (!finalize && context) ||
        binding->reserved)
        return VCO_E_ARGUMENT;
    for (unsigned i = 0; i < sizeof(binding->padding); ++i)
        if (binding->padding[i])
            return VCO_E_ARGUMENT;
    VcoIdentity identity = {.pid = binding->pid, .instance = binding->session};
    memcpy(identity.title_id, binding->title_id, 10);
    if (!vco_identity_valid(&identity))
        return VCO_E_ARGUMENT;
    memset(g, 0, sizeof(*g));
    g->binding = *binding;
    g->allocator = allocator;
    g->memory = memory;
    g->finalize = finalize;
    g->finalize_context = context;
    /* The loader-issued token is a launch identity for this current-process
     * adapter, not a sampled PID/address used to authorize another process. */
    g->session = (VcoSession){.identity = identity, .token = binding->session, .present = true};
    vco_engine_init(&g->engine);
    return VCO_OK;
}
VcoError vco_game_executor_set_prepare(VcoGameExecutor *g, VcoEngineIdlePrepare callback,
                                       void *context)
{
    if (!g || g->engine.bound || g->revoked || g->stopped || (!callback && context))
        return VCO_E_ARGUMENT;
    g->prepare = callback;
    g->prepare_context = context;
    return VCO_OK;
}
static void status(const VcoGameExecutor *g, VcoError error, VcoServiceStatus *out)
{
    *out = (VcoServiceStatus){.session = g->binding.session,
                              .revision = g->engine.bound ? g->engine.revision : 0,
                              .load_request = g->upload.request,
                              .content_key = g->engine.bound ? g->upload.content_key : 0,
                              .abi = VCO_SERVICE_ABI,
                              .size = sizeof(*out),
                              .state = error                 ? VCO_SERVICE_ERROR
                                       : g->session.runnable ? VCO_SERVICE_READY
                                                             : VCO_SERVICE_BACKGROUND,
                              .error = error,
                              .count = g->engine.bound ? g->engine.count : 0,
                              .catalog_revision = g->engine.bound ? g->upload.catalog_revision : 0,
                              .pid = g->binding.pid};
    memcpy(out->title_id, g->binding.title_id, 10);
}
static VcoError close_list(VcoGameExecutor *g)
{
    if (!g->engine.bound)
        return g->finalize ? g->finalize(g->finalize_context) : VCO_OK;
    VcoEngineCloseKey key = g->engine.close_key;
    if (!g->engine.closing) {
        if (g->engine.last_close_transaction == UINT64_MAX)
            return VCO_E_LIMIT;
        key = (VcoEngineCloseKey){.identity = g->session.identity,
                                  .session = g->binding.session,
                                  .revision = g->engine.revision,
                                  .transaction = g->engine.last_close_transaction + 1,
                                  .scope = VCO_CLOSE_LIST};
    }
    return vco_engine_close(&g->engine, &key, g->finalize, g->finalize_context, NULL);
}
static VcoError clear(VcoGameExecutor *g)
{
    VcoError error = close_list(g);
    if (error)
        return error;
    error = vco_engine_unbind(&g->engine);
    if (error)
        return error;
    if (g->arena)
        g->allocator.release(g->allocator.context, g->arena);
    g->arena = NULL;
    memset(&g->file, 0, sizeof(g->file));
    return VCO_OK;
}
static size_t aligned(size_t n)
{
    return (n + 7u) & ~(size_t)7u;
}
static VcoError module_pause(VcoGameExecutor *g)
{
    /* Close cancels queued intent and restores both published and private A
     * handles. Keep the decoded list/arena; rebind gives the UI a fresh all-OFF
     * revision. No module query or allocation is needed before the SDK call. */
    g->session.runnable = false;
    if (g->engine.bound && g->revision == UINT64_MAX)
        return VCO_E_LIMIT;
    VcoError error = close_list(g);
    if (error || !g->engine.bound)
        return error;
    error = vco_engine_unbind(&g->engine);
    if (error)
        return error;
    size_t ec = aligned(g->file.entry_capacity * sizeof(VcoCheatEntry));
    size_t cc = aligned(g->file.code_capacity * sizeof(VcoCode));
    size_t oc = aligned(g->file.code_capacity * sizeof(VcoCheatOp));
    uint8_t *arena = g->arena;
    error = vco_engine_bind(&g->engine, &g->session, ++g->revision, &g->file, g->memory,
                            (VcoCheatOp *)(arena + ec + cc), (VcoPatch *)(arena + ec + cc + oc),
                            g->file.code_capacity);
    if (!error)
        error = vco_engine_set_idle_prepare(&g->engine, g->prepare, g->prepare_context);
    return error;
}
static VcoError load(VcoGameExecutor *g, const void *packet, size_t length)
{
    VcoUploadHeader h;
    if (!packet || length < sizeof(h) || length > VCO_UPLOAD_MAX)
        return VCO_E_ARGUMENT;
    memcpy(&h, packet, sizeof(h));
    if (h.target_session != g->binding.session || h.pid != (uint32_t)g->binding.pid ||
        memcmp(h.title_id, g->binding.title_id, 10))
        return VCO_E_SESSION;
    if (h.entries > VCO_ENGINE_ENTRIES || h.codes > VCO_UPLOAD_CODES ||
        length != vco_upload_size(h.entries, h.codes))
        return VCO_E_ARGUMENT;
    if (h.request < g->upload.request ||
        (h.request == g->upload.request && memcmp(&h, &g->upload, sizeof(h))))
        return VCO_E_ARGUMENT;
    /* Decode every retry too: a copied header does not authenticate payload. */
    size_t entries = h.entries ? h.entries : 1;
    size_t codes = h.codes ? h.codes : 1;
    size_t ec = aligned(entries * sizeof(VcoCheatEntry));
    size_t cc = aligned(codes * sizeof(VcoCode));
    size_t oc = aligned(codes * sizeof(VcoCheatOp));
    size_t pc = aligned(codes * sizeof(VcoPatch));
    uint8_t *arena = g->allocator.allocate(g->allocator.context, ec + cc + oc + pc);
    if (!arena)
        return VCO_E_LIMIT;
    VcoCheatFile file = {.entries = (VcoCheatEntry *)arena,
                         .entry_capacity = entries,
                         .codes = (VcoCode *)(arena + ec),
                         .code_capacity = codes};
    VcoError error = vco_upload_decode(packet, length, &file, &h);
    if (!error && h.request == g->upload.request) {
        g->allocator.release(g->allocator.context, arena);
        return VCO_OK; /* Processed upload retry never resets an ON row. */
    }
    if (!error && g->revision == UINT64_MAX)
        error = VCO_E_LIMIT;
    if (!error)
        error = clear(g);
    if (!error) {
        error = vco_engine_bind(&g->engine, &g->session, ++g->revision, &file, g->memory,
                                (VcoCheatOp *)(arena + ec + cc), (VcoPatch *)(arena + ec + cc + oc),
                                codes);
        if (!error) {
            error = vco_engine_set_idle_prepare(&g->engine, g->prepare, g->prepare_context);
        }
        if (!error) {
            g->arena = arena;
            g->file = file;
            g->upload = h;
            return VCO_OK;
        }
        if (g->engine.bound)
            (void)vco_engine_unbind(&g->engine);
    }
    g->allocator.release(g->allocator.context, arena);
    return error;
}
void vco_game_executor_revoke(VcoGameExecutor *g)
{
    if (g) {
        g->revoked = true;
        g->session.runnable = false;
    }
}
VcoError vco_game_executor_handle(VcoGameExecutor *g, const VcoAgentMessage *input,
                                  const void *payload, size_t length, uint64_t now,
                                  VcoAgentResult *out)
{
    if (!g || !input || !out)
        return VCO_E_ARGUMENT;
    VcoAgentMessage m;
    memcpy(&m, input, sizeof(m));
    if (g->revoked || g->stopped || m.session != g->binding.session)
        return VCO_E_SESSION;
    if (m.abi != VCO_AGENT_ABI || m.size != sizeof(m) || m.reserved || m.runnable > 1 ||
        m.length != length || m.operation > VCO_AGENT_MODULE_PAUSE ||
        (m.operation == VCO_AGENT_IDLE ? m.ticket || length : !m.ticket) ||
        ((m.operation == VCO_AGENT_CLEAR || m.operation == VCO_AGENT_MODULE_PAUSE) && length) ||
        (m.operation == VCO_AGENT_MODULE_PAUSE && m.runnable) ||
        (m.operation == VCO_AGENT_CONTROL && length != sizeof(VcoControlRequest)) ||
        (length && !payload))
        return VCO_E_ARGUMENT;
    if (m.ticket && m.ticket < g->last_ticket)
        return VCO_E_SESSION;
    g->session.runnable = m.runnable != 0;
    if (m.ticket && m.ticket == g->last_ticket) {
        if (m.operation != g->last_result.operation)
            return VCO_E_ARGUMENT;
        /* The command itself is cached, but fresh poll authority still pauses
         * or advances periodic execution while its reply awaits delivery. */
        vco_engine_step(&g->engine, now);
        *out = g->last_result;
        return VCO_OK;
    }
    VcoAgentResult result = {.session = m.session,
                             .ticket = m.ticket,
                             .abi = VCO_AGENT_ABI,
                             .size = sizeof(result),
                             .operation = m.operation};
    VcoError error = VCO_OK;
    if (m.operation == VCO_AGENT_LOAD)
        error = load(g, payload, length);
    else if (m.operation == VCO_AGENT_CLEAR)
        error = clear(g);
    else if (m.operation == VCO_AGENT_MODULE_PAUSE)
        error = module_pause(g);
    else if (m.operation == VCO_AGENT_CONTROL) {
        VcoControlStatus control = vco_control_dispatch(&g->engine, g->binding.pid, g->binding.pid,
                                                        payload, length, &result.control);
        error = control == VCO_CONTROL_OK                                        ? VCO_OK
                : control == VCO_CONTROL_STALE || control == VCO_CONTROL_UNBOUND ? VCO_E_SESSION
                : result.control.error ? (VcoError)result.control.error
                                       : VCO_E_ARGUMENT;
    }
    if (!error)
        vco_engine_step(&g->engine, now);
    result.error = error;
    status(g, error, &result.status);
    if (m.ticket) {
        g->last_ticket = m.ticket;
        g->last_result = result;
    }
    *out = result;
    return VCO_OK; /* Operation errors travel in the copied result. */
}
VcoError vco_game_executor_stop(VcoGameExecutor *g)
{
    if (!g)
        return VCO_E_ARGUMENT;
    if (g->stopped)
        return VCO_OK;
    if (g->revoked)
        return VCO_E_SESSION;
    VcoError error = clear(g);
    if (!error) {
        g->stopped = true;
        g->session.runnable = false;
    }
    return error;
}
VcoError vco_game_executor_stopped_result(const VcoGameExecutor *g, VcoAgentResult *out)
{
    if (!g || !out)
        return VCO_E_ARGUMENT;
    if (g->revoked || !g->stopped || g->arena || g->engine.bound || g->engine.blocked_restore)
        return VCO_E_SESSION;
    *out = (VcoAgentResult){.session = g->binding.session,
                            .abi = VCO_AGENT_ABI,
                            .size = sizeof(*out),
                            .operation = VCO_AGENT_STOPPED};
    status(g, VCO_OK, &out->status);
    return VCO_OK;
}
