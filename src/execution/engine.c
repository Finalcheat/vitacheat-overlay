#include "engine.h"
#include <string.h>
enum { CLAIM_DATA = 1u, CLAIM_PATCH = 2u };
static uint32_t entry_budget(size_t physical_lines)
{
    if (physical_lines > VCO_ENGINE_ENTRY_BUDGET_MAX / VCO_ENGINE_BUDGET_PER_CODE)
        return VCO_ENGINE_ENTRY_BUDGET_MAX;
    uint32_t scaled = (uint32_t)physical_lines * VCO_ENGINE_BUDGET_PER_CODE;
    return scaled < VCO_ENGINE_ENTRY_BUDGET ? VCO_ENGINE_ENTRY_BUDGET : scaled;
}
static bool current(const VcoEngine *e, bool foreground)
{
    return e->bound && !e->closing && vco_session_matches(e->session, e->token, foreground);
}
static int alive(void *ctx, uint64_t token)
{
    VcoExecutionSlot *s = ctx;
    VcoEngine *e = s->engine;
    return token == e->token && current(e, true) && e->memory.alive &&
           e->memory.alive(e->memory.context, token);
}
static void forget_claims(VcoEngine *e, unsigned entry)
{
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i)
        if (e->claims[i].flags && e->claims[i].entry == entry)
            e->claims[i].flags = 0;
}
static void refresh_data_claims(VcoEngine *e, unsigned entry)
{
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i)
        if (e->claims[i].entry == entry)
            e->claims[i].flags &= (uint8_t)~CLAIM_DATA;
}
static void release_patch_claim(VcoExecutionSlot *s, uint32_t address, uint8_t width)
{
    VcoEngine *e = s->engine;
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i) {
        VcoWriteClaim *c = &e->claims[i];
        if (c->entry == s->index && c->address == address && c->width == width)
            c->flags &= (uint8_t)~CLAIM_PATCH;
    }
}
static VcoError claim(VcoExecutionSlot *s, uint32_t address, size_t size, uint8_t flags)
{
    if (!size || size > 4 || (uint64_t)address + size > UINT64_C(0x100000000))
        return VCO_E_ADDRESS;
    VcoEngine *e = s->engine;
    unsigned free_index = VCO_ENGINE_CLAIMS;
    unsigned owned = VCO_ENGINE_CLAIMS;
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i) {
        VcoWriteClaim *c = &e->claims[i];
        if (!c->flags) {
            if (free_index == VCO_ENGINE_CLAIMS)
                free_index = i;
            continue;
        }
        if (c->entry != s->index && (uint64_t)address < (uint64_t)c->address + c->width &&
            (uint64_t)c->address < (uint64_t)address + size)
            return VCO_E_PATCH;
        if (c->entry == s->index && c->address == address && c->width == size)
            owned = i;
    }
    if (owned != VCO_ENGINE_CLAIMS) {
        e->claims[owned].flags |= flags;
        return 0;
    }
    if (free_index == VCO_ENGINE_CLAIMS)
        return VCO_E_LIMIT;
    e->claims[free_index] = (VcoWriteClaim){address, (uint16_t)s->index, (uint8_t)size, flags};
    return 0;
}
static int read_memory(void *ctx, uint64_t token, uint32_t address, uint8_t *out, size_t size)
{
    VcoExecutionSlot *s = ctx;
    VcoCheatMemory *m = &s->engine->memory;
    return !alive(ctx, token) ? VCO_ACCESS_SESSION
           : !m->read         ? -1
                              : m->read(m->context, token, address, out, size);
}
static int write_memory(void *ctx, uint64_t token, uint32_t address, const uint8_t *in, size_t size)
{
    VcoExecutionSlot *s = ctx;
    VcoCheatMemory *m = &s->engine->memory;
    if (!alive(ctx, token))
        return VCO_ACCESS_SESSION;
    if (!m->write)
        return -1;
    s->access_error = claim(s, address, size, CLAIM_DATA);
    return s->access_error ? -1 : m->write(m->context, token, address, in, size);
}
static int module(void *ctx, uint64_t token, unsigned index, unsigned segment, uint32_t *base,
                  uint32_t *size)
{
    VcoExecutionSlot *s = ctx;
    VcoCheatMemory *m = &s->engine->memory;
    return !alive(ctx, token) ? VCO_ACCESS_SESSION
           : !m->module       ? -1
                              : m->module(m->context, token, index, segment, base, size);
}
static int pad(void *ctx, uint64_t token, uint32_t *device, uint32_t *buttons)
{
    VcoExecutionSlot *s = ctx;
    VcoCheatMemory *m = &s->engine->memory;
    return !alive(ctx, token) ? VCO_ACCESS_SESSION
           : !m->pad          ? -1
                              : m->pad(m->context, token, device, buttons);
}
static int inject(void *ctx, uint64_t token, uint32_t address, const uint8_t *in, size_t size,
                  int32_t *handle)
{
    VcoExecutionSlot *s = ctx;
    VcoCheatMemory *m = &s->engine->memory;
    if (!alive(ctx, token))
        return VCO_ACCESS_SESSION;
    if (!m->inject)
        return -1;
    s->access_error = claim(s, address, size, CLAIM_PATCH);
    return s->access_error ? -1 : m->inject(m->context, token, address, in, size, handle);
}
static int release(void *ctx, uint64_t token, int32_t handle)
{
    VcoExecutionSlot *s = ctx;
    VcoCheatMemory *m = &s->engine->memory;
    /* Cleanup must run even after foreground/identity loss. Adapter consumes the
     * handle and is responsible for never restoring into a replaced process. */
    const VcoPatch *receipt = NULL;
    for (size_t i = 0; i < s->runtime.program->count; ++i)
        if (s->runtime.patches[i].active && s->runtime.patches[i].handle == handle) {
            receipt = &s->runtime.patches[i];
            break;
        }
    int result = !m->release ? -1 : m->release(m->context, token, handle);
    if (!result && receipt)
        release_patch_claim(s, receipt->address, receipt->width);
    return result;
}
static bool supported(const VcoCheatProgram *p, const VcoCheatMemory *m)
{
    if (p->needs_review)
        return false;
    for (size_t i = 0; i < p->count; ++i) {
        VcoOpKind kind = p->ops[i].kind;
        if (kind == VCO_OP_MODULE) {
            if (!m->module)
                return false;
        } else if (kind == VCO_OP_PAD) {
            if (!m->pad)
                return false;
        } else if (kind == VCO_OP_PATCH) {
            if (!m->inject || !m->release)
                return false;
        } else {
            if (kind != VCO_OP_COMPARE && !m->write)
                return false;
            if (kind != VCO_OP_WRITE && kind != VCO_OP_REPEAT && !m->read)
                return false;
        }
    }
    return true;
}
void vco_engine_init(VcoEngine *e)
{
    memset(e, 0, sizeof(*e));
}
static VcoError stop(VcoEngine *e, unsigned index)
{
    VcoExecutionSlot *s = &e->slots[index];
    VcoError error = s->runtime.program ? vco_cheat_disable(&s->runtime) : VCO_OK;
    if (error == VCO_E_RESTORE)
        e->blocked_restore = true;
    if (!error)
        forget_claims(e, index);
    return error;
}
VcoError vco_engine_unbind(VcoEngine *e)
{
    if (!e)
        return VCO_E_ARGUMENT;
    if (!e->closing)
        for (unsigned i = 0; i < e->count; ++i)
            (void)stop(e, i);
    bool blocked = e->blocked_restore;
    uint64_t last_revision = e->last_revision;
    uint64_t last_close_transaction = e->last_close_transaction;
    memset(e, 0, sizeof(*e));
    e->blocked_restore = blocked;
    e->last_revision = last_revision;
    e->last_close_transaction = last_close_transaction;
    return blocked ? VCO_E_RESTORE : VCO_OK;
}
VcoError vco_engine_bind(VcoEngine *e, const VcoSession *session, uint64_t revision,
                         const VcoCheatFile *file, VcoCheatMemory memory, VcoCheatOp *ops,
                         VcoPatch *patches, size_t capacity)
{
    if (!e || !session || !vco_session_matches(session, session->token, false) || !revision ||
        revision <= e->last_revision || !file || !file->entries || !file->codes ||
        file->entry_count > VCO_ENGINE_ENTRIES || !memory.alive || !ops || !patches || !capacity)
        return VCO_E_ARGUMENT;
    if (e->closing && (!e->close_complete || e->blocked_restore))
        return VCO_E_RESTORE; /* A failed replacement cannot erase its close evidence. */
    VcoError error = vco_engine_unbind(e);
    if (error)
        return error;
    e->session = session;
    e->identity = session->identity;
    e->memory = memory;
    e->token = session->token;
    e->revision = revision;
    e->last_revision = revision;
    e->count = (unsigned)file->entry_count;
    e->bound = true;
    size_t used = 0;
    for (unsigned i = 0; i < e->count; ++i) {
        VcoExecutionSlot *s = &e->slots[i];
        VcoExecutionRow *row = &e->rows[i];
        s->engine = e;
        s->index = i;
        s->program = (VcoCheatProgram){ops + used, capacity - used, 0, NULL, 0};
        error = vco_cheat_compile(file, i, &s->program, &row->diagnostic);
        if (!error && !supported(&s->program, &memory)) {
            error = VCO_E_UNVERIFIED;
            row->diagnostic.code = error;
        }
        if (!error) {
            VcoCheatMemory proxy = {s,      alive, read_memory, write_memory,
                                    module, pad,   inject,      release};
            error =
                vco_cheat_runtime_init(&s->runtime, &s->program, proxy, e->token, patches + used,
                                       capacity - used, entry_budget(file->entries[i].count), 0);
            if (error)
                row->diagnostic.code = error;
        }
        row->state = error ? VCO_ENTRY_UNSUPPORTED : VCO_ENTRY_OFF;
        used += s->program.count;
        /* file_enabled is deliberately ignored: all new lists start OFF. */
    }
    return VCO_OK;
}
VcoError vco_engine_submit(VcoEngine *e, VcoExecutionCommand c)
{
    if (!e || !c.request || c.entry >= e->count)
        return VCO_E_ARGUMENT;
    if (!current(e, false) || c.session != e->token || c.revision != e->revision)
        return VCO_E_SESSION;
    if (e->blocked_restore)
        return VCO_E_RESTORE;
    VcoExecutionSlot *s = &e->slots[c.entry];
    VcoExecutionRow *r = &e->rows[c.entry];
    if (c.request == s->last_request)
        return c.enabled == s->last_desired ? VCO_OK : VCO_E_ARGUMENT;
    if (c.request < s->last_request)
        return VCO_E_ARGUMENT;
    if (r->state == VCO_ENTRY_UNSUPPORTED)
        return r->diagnostic.code;
    if (c.enabled && !current(e, true))
        return VCO_E_SESSION;
    if (r->pending || e->queued == VCO_ENGINE_QUEUE)
        return VCO_E_LIMIT;
    e->commands[(e->head + e->queued++) % VCO_ENGINE_QUEUE] = c;
    s->last_request = c.request;
    s->last_desired = c.enabled;
    r->pending = c.request;
    r->state = c.enabled ? VCO_ENTRY_ENABLING : VCO_ENTRY_DISABLING;
    return VCO_OK;
}
static bool idle(const VcoEngine *e)
{
    if (e->blocked_restore)
        return false;
    for (unsigned i = 0; i < e->count; ++i) {
        const VcoCheatRuntime *r = &e->slots[i].runtime;
        if (e->rows[i].desired || r->state == VCO_CHEAT_ON)
            return false;
        if (r->program)
            for (size_t j = 0; j < r->program->count; ++j)
                if (r->patches[j].active)
                    return false;
    }
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i)
        if (e->claims[i].flags)
            return false;
    return true;
}
VcoError vco_engine_set_idle_prepare(VcoEngine *e, VcoEngineIdlePrepare prepare, void *context)
{
    if (!e || !e->bound || e->closing || e->queued || !idle(e))
        return VCO_E_ARGUMENT;
    e->idle_prepare = prepare;
    e->idle_prepare_context = context;
    return VCO_OK;
}
static void finish(VcoEngine *e, unsigned index, VcoError error, bool desired)
{
    VcoExecutionRow *r = &e->rows[index];
    VcoCheatRuntime *runtime = &e->slots[index].runtime;
    if (error == VCO_E_SESSION && desired && current(e, false)) {
        VcoError cleanup = stop(e, index);
        if (!cleanup) {
            /* Retain explicit user intent only for this same session. Every
             * resumed tick still passes all native authority checks. */
            r->desired = true;
            r->state = VCO_ENTRY_PAUSED;
            r->diagnostic = (VcoDiagnostic){VCO_OK, 0};
            e->scheduled = false;
            return;
        }
        error = cleanup;
    }
    r->desired = !error && desired;
    r->state = error ? VCO_ENTRY_ERROR : desired ? VCO_ENTRY_ON : VCO_ENTRY_OFF;
    r->diagnostic =
        error ? (runtime->diagnostic.code ? runtime->diagnostic : (VcoDiagnostic){error, 0})
              : (VcoDiagnostic){VCO_OK, 0};
    if (error != VCO_E_RESTORE && error && e->slots[index].access_error)
        r->diagnostic.code = e->slots[index].access_error;
    if (error == VCO_E_RESTORE)
        e->blocked_restore = true;
    if (error && error != VCO_E_RESTORE)
        forget_claims(e, index);
}
static bool same_identity(const VcoIdentity *a, const VcoIdentity *b)
{
    return a->pid == b->pid && a->instance == b->instance &&
           !memcmp(a->title_id, b->title_id, sizeof(a->title_id));
}
static VcoError close_error(const VcoEngineCloseResult *result)
{
    return result->entries ? result->entries : result->native;
}
VcoError vco_engine_close(VcoEngine *e, const VcoEngineCloseKey *key, VcoEngineCloseNative finalize,
                          void *context, VcoEngineCloseResult *result)
{
    if (!e || !vco_close_key_valid(key) || (!finalize && context))
        return VCO_E_ARGUMENT;
    if (!e->bound || key->session != e->token || key->revision != e->revision ||
        !same_identity(&key->identity, &e->identity))
        return VCO_E_SESSION;
    if (e->closing) {
        if (!vco_close_key_equal(key, &e->close_key))
            return VCO_E_SESSION;
        if (!e->close_complete || finalize != e->close_native || context != e->close_native_context)
            return VCO_E_ARGUMENT;
    } else {
        if (key->transaction <= e->last_close_transaction)
            return VCO_E_SESSION;
        /* No normal execution can start after this point. Retain the list and
         * diagnostic evidence until both public and private owners report. */
        e->closing = true;
        e->close_key = *key;
        e->last_close_transaction = key->transaction;
        e->close_native = finalize;
        e->close_native_context = context;
        e->head = e->queued = 0;
        e->scheduled = false;
        e->close_result.entries = e->blocked_restore ? VCO_E_RESTORE : VCO_OK;
        for (unsigned i = 0; i < e->count; ++i) {
            VcoExecutionRow *row = &e->rows[i];
            row->desired = false;
            row->pending = 0; /* Cancelled ON is never acknowledged as executed. */
            VcoError error = stop(e, i);
            if (error && !e->close_result.entries)
                e->close_result.entries = error;
            if (row->state != VCO_ENTRY_UNSUPPORTED)
                finish(e, i, error, false);
        }
        e->close_result.native = finalize ? finalize(context) : VCO_OK;
        if (e->close_result.native)
            e->blocked_restore = true;
        e->close_complete = true;
    }
    if (result)
        *result = e->close_result;
    return close_error(&e->close_result);
}
VcoError vco_engine_reopen(VcoEngine *e, const VcoEngineCloseKey *key, uint64_t revision)
{
    if (!e || !key || !revision || !e->bound || !e->closing || !e->close_complete)
        return VCO_E_ARGUMENT;
    if (!vco_close_key_equal(key, &e->close_key) ||
        !vco_session_matches(e->session, e->token, false) ||
        !same_identity(&e->identity, &e->session->identity))
        return VCO_E_SESSION;
    VcoError error = close_error(&e->close_result);
    if (error)
        return error;
    if (!idle(e))
        return VCO_E_RESTORE;
    if (revision <= e->last_revision)
        return VCO_E_ARGUMENT;
    e->revision = e->last_revision = revision;
    e->last_tick = 0;
    e->closing = e->close_complete = false;
    memset(&e->close_key, 0, sizeof(e->close_key));
    e->close_result = (VcoEngineCloseResult){VCO_OK, VCO_OK};
    e->close_native = NULL;
    e->close_native_context = NULL;
    for (unsigned i = 0; i < e->count; ++i) {
        e->slots[i].last_request = 0;
        e->slots[i].last_desired = false;
        e->slots[i].access_error = VCO_OK;
        e->rows[i].acknowledged = 0;
    }
    return VCO_OK;
}
void vco_engine_step(VcoEngine *e, uint64_t now_us)
{
    if (!e || !e->bound || e->closing)
        return;
    if (!current(e, false)) {
        (void)vco_engine_unbind(e);
        return;
    }
    bool activated[VCO_ENGINE_ENTRIES] = {false};
    /* Consume commands before periodic writes: confirmed OFF has no later tick. */
    while (e->queued) {
        VcoExecutionCommand c = e->commands[e->head];
        e->head = (e->head + 1) % VCO_ENGINE_QUEUE;
        --e->queued;
        VcoExecutionRow *r = &e->rows[c.entry];
        VcoError error = VCO_OK;
        e->slots[c.entry].access_error = VCO_OK;
        if (c.enabled) {
            unsigned active = 0;
            for (unsigned i = 0; i < e->count; ++i)
                if (e->rows[i].desired)
                    ++active;
            if (e->blocked_restore)
                error = VCO_E_RESTORE;
            else if (!r->desired && active >= VCO_ENGINE_ACTIVE)
                error = VCO_E_LIMIT;
            else if (!current(e, true))
                error = VCO_E_SESSION;
            else {
                if (e->idle_prepare && idle(e))
                    e->idle_prepare(e->idle_prepare_context, e->token);
                if (e->slots[c.entry].runtime.state != VCO_CHEAT_ON)
                    refresh_data_claims(e, c.entry);
                error = vco_cheat_enable(&e->slots[c.entry].runtime);
            }
            activated[c.entry] = true;
        } else
            error = stop(e, c.entry);
        if (error && e->slots[c.entry].runtime.state == VCO_CHEAT_ON) {
            VcoError cleanup = stop(e, c.entry);
            if (cleanup)
                error = cleanup;
        }
        finish(e, c.entry, error, c.enabled);
        r->acknowledged = c.request;
        r->pending = 0;
    }
    if (!current(e, true) || e->blocked_restore) {
        for (unsigned i = 0; i < e->count; ++i) {
            VcoExecutionRow *r = &e->rows[i];
            if (!r->desired || r->state == VCO_ENTRY_PAUSED)
                continue;
            VcoError error = stop(e, i);
            if (error)
                finish(e, i, error, false);
            else
                r->state = VCO_ENTRY_PAUSED;
        }
        e->scheduled = false;
        return;
    }
    bool due =
        !e->scheduled || now_us < e->last_tick || now_us - e->last_tick >= VCO_ENGINE_PERIOD_US;
    if (!due)
        return;
    e->last_tick = now_us;
    e->scheduled = true;
    for (unsigned i = 0; i < e->count; ++i) {
        VcoExecutionRow *r = &e->rows[i];
        if (!r->desired || activated[i])
            continue;
        e->slots[i].access_error = VCO_OK;
        refresh_data_claims(e, i);
        VcoError error = r->state == VCO_ENTRY_PAUSED ? vco_cheat_enable(&e->slots[i].runtime)
                                                      : vco_cheat_tick(&e->slots[i].runtime);
        finish(e, i, error, true);
        if (e->blocked_restore)
            break;
    }
}
