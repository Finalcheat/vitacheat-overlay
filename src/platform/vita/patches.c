#include "patches.h"
#include "memblock_lookup.h"
#include <psp2/kernel/error.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
#include <string.h>
#include <limits.h>
static void trace(const VcoVitaPatchSupport *s, uint32_t event, int32_t result, uint32_t subject,
                  uint32_t detail)
{
    if (s && s->trace)
        s->trace(s->trace_context, event, result, subject, detail);
}
static void probe_result(const VcoVitaPatchSupport *s)
{
    uint32_t flags = (s->ready ? 1u : 0u) | (s->quarantined ? 2u : 0u) | (s->referenced ? 4u : 0u) |
                     (s->probe_uid >= 0 ? 8u : 0u);
    trace(s, VCO_PATCH_TRACE_PROBE_RESULT, s->ready ? 0 : -1, (uint32_t)s->closed_error, flags);
}
static bool uid_error(int result)
{
    /* Named SDK UID-absence errors only. A generic injected/transient failure
     * must not become the closed-UID capability merely by occurring in probe. */
    return (uint32_t)result == SCE_KERNEL_ERROR_INVALID_UID ||
           (uint32_t)result == SCE_KERNEL_ERROR_UNKNOWN_UID;
}
void vco_vita_patch_support_init(VcoVitaPatchSupport *s)
{
    memset(s, 0, sizeof(*s));
    s->probe_uid = -1;
}
int vco_vita_patch_support_set_trace(VcoVitaPatchSupport *s, VcoVitaPatchTrace callback,
                                     void *context)
{
    if (!s || s->attempted)
        return -1;
    s->trace = callback;
    s->trace_context = callback ? context : NULL;
    return 0;
}
int vco_vita_patch_support_probe(VcoVitaPatchSupport *s)
{
    if (!s)
        return -1;
    if (s->attempted)
        return s->ready ? 0 : -1;
    s->attempted = true;
    s->probe_uid = ksceKernelCreateEventFlag("VCOGuidProbe", 0, 0, NULL);
    trace(s, VCO_PATCH_TRACE_PROBE_CREATE, s->probe_uid, 0, 0);
    if (s->probe_uid < 0) {
        probe_result(s);
        return -1;
    }
    SceObjectBase *original = NULL;
    int reference = ksceGUIDReferObject(s->probe_uid, &original);
    trace(s, VCO_PATCH_TRACE_PROBE_REFERENCE, reference, (uint32_t)s->probe_uid, original != NULL);
    if (reference >= 0) {
        s->referenced = true;
        s->probe_object = original;
    }
    int closed = ksceKernelDeleteEventFlag(s->probe_uid);
    trace(s, VCO_PATCH_TRACE_PROBE_DELETE, closed, (uint32_t)s->probe_uid, 0);
    bool verified = closed >= 0 && s->referenced && original;
    if (closed < 0)
        s->quarantined = true;
    if (verified) {
        for (unsigned i = 0; i < 2; ++i) {
            SceObjectBase *current = NULL;
            int result = ksceGUIDReferObject(s->probe_uid, &current);
            trace(s, VCO_PATCH_TRACE_PROBE_CLOSED_QUERY, result, (uint32_t)s->probe_uid, i);
            if (result >= 0) {
                /* Some firmware could keep closed UIDs referenceable. That is
                 * not the capability needed here, but balance this reference. */
                int released = ksceGUIDReleaseObject(s->probe_uid);
                trace(s, VCO_PATCH_TRACE_PROBE_RELEASE, released, (uint32_t)s->probe_uid, 0);
                if (released < 0)
                    s->quarantined = true;
                verified = false;
                break;
            }
            if (!uid_error(result) || (i && result != s->closed_error)) {
                verified = false;
                break;
            }
            s->closed_error = result;
        }
    }
    if (s->referenced) {
        s->referenced = false; /* Consumed even when release is ambiguous. */
        int released = ksceGUIDReleaseObject(s->probe_uid);
        trace(s, VCO_PATCH_TRACE_PROBE_RELEASE, released, (uint32_t)s->probe_uid, 1);
        if (released < 0)
            s->quarantined = true;
    }
    if (closed >= 0 && !s->quarantined) {
        s->probe_uid = -1;
        s->probe_object = NULL;
    }
    s->ready = verified && !s->quarantined;
    probe_result(s);
    return s->ready ? 0 : -1;
}
int vco_vita_patch_support_dispose(const VcoVitaPatchSupport *s)
{
    return !s || s->referenced || s->quarantined || s->probe_uid >= 0 ? -1 : 0;
}
static bool same_identity(const VcoIdentity *a, const VcoIdentity *b)
{
    return a && b && a->pid == b->pid && a->instance == b->instance &&
           !memcmp(a->title_id, b->title_id, sizeof(a->title_id));
}
static void quarantine(VcoVitaExecution *e, VcoVitaPatch *p, uint32_t reason)
{
    if (!p->quarantined)
        trace(e->support, VCO_PATCH_TRACE_QUARANTINE, -1, (uint32_t)p->key, reason);
    p->quarantined = true;
}
static int poison(VcoVitaExecution *e, VcoVitaPatch *p)
{
    p->active = false;
    quarantine(e, p, VCO_PATCH_TRACE_REASON_UNCERTAIN);
    if (!e->poisoned)
        trace(e->support, VCO_PATCH_TRACE_POISON, -1, (uint32_t)p->key, 0);
    e->blocked = e->poisoned = true;
    return -1;
}
static int drop_reference(VcoVitaExecution *e, VcoVitaPatch *p)
{
    if (!p->referenced || !p->object)
        return poison(e, p);
    p->referenced = false;
    int result = ksceGUIDReleaseObject(p->tai_handle);
    trace(e->support, VCO_PATCH_TRACE_PATCH_RELEASE_REFERENCE, result, (uint32_t)p->tai_handle,
          (uint32_t)p->key);
    if (result < 0)
        return poison(e, p);
    p->object = NULL;
    return 0;
}
static int drop_block_reference(VcoVitaExecution *e, VcoVitaPatch *p)
{
    if (!p->block_referenced)
        return poison(e, p);
    p->block_referenced = false;
    int result = ksceGUIDReleaseObject(p->memblock);
    trace(e->support, VCO_PATCH_TRACE_BLOCK_RELEASE_REFERENCE, result, (uint32_t)p->memblock,
          (uint32_t)p->key);
    if (result < 0)
        return poison(e, p);
    p->block_object = NULL;
    return 0;
}
/* This read-only mapping/object observation requires the original retained
 * block reference, not a range lock. Write/restore callers still hold their
 * range references; publication also checks after unlock. A changed mapping
 * is quarantined; identity/release ambiguity is permanent. Never release
 * taiHEN on either. This observation is not an atomic lifetime guarantee. */
static int same_block(VcoVitaExecution *e, VcoVitaPatch *p)
{
    if (!p->block_referenced || !p->block_object)
        return poison(e, p);
    SceUID current = vco_vita_find_memblock(e->identity.pid, p->address, p->size);
    if (current < 0 || current != p->memblock) {
        trace(e->support, VCO_PATCH_TRACE_BLOCK_MAPPING_FAILURE, current, (uint32_t)p->memblock,
              (uint32_t)p->key);
        return -1;
    }
    SceObjectBase *object = NULL;
    int reference = ksceGUIDReferObject(current, &object);
    if (reference < 0) {
        trace(e->support, VCO_PATCH_TRACE_BLOCK_QUERY, reference, (uint32_t)current,
              (uint32_t)p->key);
        return -1;
    }
    bool match = object && object == p->block_object;
    int result = ksceGUIDReleaseObject(current);
    if (!match)
        trace(e->support, VCO_PATCH_TRACE_BLOCK_QUERY, reference, (uint32_t)current,
              (uint32_t)p->key);
    if (result < 0)
        trace(e->support, VCO_PATCH_TRACE_TEMP_RELEASE_FAILURE, result, (uint32_t)current,
              (uint32_t)p->key);
    return result < 0 || !match ? poison(e, p) : 0;
}
/* 0=same live object, 1=closed under the observed firmware contract, -1=unknown.
 * The retained original reference is not dropped by this observation. */
static int native_state(VcoVitaExecution *e, VcoVitaPatch *p)
{
    if (!e->support || !e->support->ready || !p->referenced || !p->object || e->poisoned)
        return poison(e, p);
    SceObjectBase *current = NULL;
    int result = ksceGUIDReferObject(p->tai_handle, &current);
    if (result < 0) {
        trace(e->support,
              result == e->support->closed_error ? VCO_PATCH_TRACE_NATIVE_CLOSED
                                                 : VCO_PATCH_TRACE_NATIVE_QUERY_FAILURE,
              result, (uint32_t)p->tai_handle, (uint32_t)p->key);
        return result == e->support->closed_error ? 1 : poison(e, p);
    }
    bool match = current && current == p->object;
    int released = ksceGUIDReleaseObject(p->tai_handle);
    if (!match)
        trace(e->support, VCO_PATCH_TRACE_NATIVE_QUERY_FAILURE, result, (uint32_t)p->tai_handle,
              (uint32_t)p->key);
    if (released < 0)
        trace(e->support, VCO_PATCH_TRACE_TEMP_RELEASE_FAILURE, released, (uint32_t)p->tai_handle,
              (uint32_t)p->key);
    if (released < 0 || !match)
        return poison(e, p);
    return 0;
}
static int retire_block_reference(VcoVitaExecution *e, VcoVitaPatch *p)
{
    if (!p->block_referenced || !p->block_object)
        return poison(e, p);
    SceObjectBase *current = NULL;
    int result = ksceGUIDReferObject(p->memblock, &current);
    trace(e->support, VCO_PATCH_TRACE_BLOCK_QUERY, result, (uint32_t)p->memblock, (uint32_t)p->key);
    if (result >= 0) {
        bool match = current && current == p->block_object;
        int released = ksceGUIDReleaseObject(p->memblock);
        if (released < 0)
            trace(e->support, VCO_PATCH_TRACE_TEMP_RELEASE_FAILURE, released, (uint32_t)p->memblock,
                  (uint32_t)p->key);
        if (released < 0 || !match)
            return poison(e, p);
    } else if (result != e->support->closed_error) {
        return poison(e, p);
    }
    return drop_block_reference(e, p);
}
static int alive(void *context, uint64_t token)
{
    VcoVitaExecution *e = context;
    VcoCheatMemory memory = vco_memory_callbacks(&e->data);
    return !e->blocked && memory.alive(memory.context, token);
}
static int read_data(void *context, uint64_t token, uint32_t addr, uint8_t *out, size_t size)
{
    VcoVitaExecution *e = context;
    if (e->blocked || (e->admission && !vco_admission_enter(e->admission)))
        return -1;
    VcoCheatMemory memory = vco_memory_callbacks(&e->data);
    int result = memory.read(memory.context, token, addr, out, size);
    if (result)
        trace(e->support, VCO_MEMORY_TRACE_READ_FAILURE, result, addr, (uint32_t)size);
    if (e->admission)
        (void)vco_admission_leave(e->admission);
    return result;
}
static int write_data(void *context, uint64_t token, uint32_t addr, const uint8_t *in, size_t size)
{
    VcoVitaExecution *e = context;
    if (e->blocked || (e->admission && !vco_admission_enter(e->admission)))
        return -1;
    VcoCheatMemory memory = vco_memory_callbacks(&e->data);
    int result = memory.write(memory.context, token, addr, in, size);
    if (result)
        trace(e->support, VCO_MEMORY_TRACE_WRITE_FAILURE, result, addr, (uint32_t)size);
    if (e->admission)
        (void)vco_admission_leave(e->admission);
    return result;
}
static int resolve_module(void *context, uint64_t token, unsigned index, unsigned segment,
                          uint32_t *base, uint32_t *size)
{
    VcoVitaExecution *e = context;
    if (!base || !size || base == size || !e->features.module)
        return -1;
    if (!alive(e, token))
        return VCO_ACCESS_SESSION;
    uint32_t staged_base = 0, staged_size = 0;
    int result = e->features.module(e->features.module_context, token, index, segment, &staged_base,
                                    &staged_size);
    if (result)
        return result == VCO_ACCESS_SESSION ? VCO_ACCESS_SESSION : -1;
    if (!alive(e, token))
        return VCO_ACCESS_SESSION;
    if (staged_base < 0x40000000u || !staged_size ||
        (uint64_t)staged_base + staged_size > UINT64_C(0x100000000))
        return -1;
    *base = staged_base;
    *size = staged_size;
    return 0;
}
static int sample_pad(void *context, uint64_t token, uint32_t *device, uint32_t *buttons)
{
    VcoVitaExecution *e = context;
    if (!device || !buttons || device == buttons || !e->features.pad)
        return -1;
    if (!alive(e, token))
        return VCO_ACCESS_SESSION;
    uint32_t staged_device = 0, staged_buttons = 0;
    int result = e->features.pad(e->features.pad_context, token, &staged_device, &staged_buttons);
    if (result && result != VCO_PAD_SUPPRESSED)
        return result == VCO_ACCESS_SESSION ? VCO_ACCESS_SESSION : -1;
    if (!alive(e, token))
        return VCO_ACCESS_SESSION;
    if (result)
        return VCO_PAD_SUPPRESSED;
    *device = staged_device;
    *buttons = staged_buttons;
    return 0;
}
static int guard_module(VcoVitaExecution *e, uint64_t token, uint32_t address, uint32_t size,
                        uint64_t *generation, bool capture)
{
    if (!e->features.module_guard)
        return *generation ? -1 : 0;
    uint64_t copy = *generation;
    if (e->features.module_guard(e->features.module_context, token, address, size, &copy) ||
        (!capture && copy != *generation))
        return -1;
    *generation = copy;
    return 0;
}
static void reject_injected_owner(VcoVitaExecution *e, VcoVitaPatch *p)
{
    /* A native write happened, but its target ownership was not confirmed. No
     * public handle/ON result and no speculative rollback into an unknown owner.
     * Keep both original references for the existing trusted-retirement path. */
    p->active = false;
    quarantine(e, p, VCO_PATCH_TRACE_REASON_INJECT_GUARD);
    if (!e->blocked)
        trace(e->support, VCO_PATCH_TRACE_BLOCKED, -1, (uint32_t)p->key,
              VCO_PATCH_TRACE_REASON_INJECT_GUARD);
    e->blocked = true;
}
static int restore_admitted(VcoVitaExecution *e, VcoVitaPatch *p);
typedef struct {
    uint32_t stage;
    int32_t result;
} InjectFailure;
/* Observe exactly the first failing check, without changing its value, short
 * circuit order, native calls or cleanup. Later unlock/cleanup cannot overwrite
 * the original cause; their existing raw records still precede the summary. */
static int failed_at(InjectFailure *failure, uint32_t stage, int result)
{
    if (result && !failure->stage)
        *failure = (InjectFailure){stage, result};
    return result;
}
static bool clean_preinject_denial(const VcoVitaExecution *e, const InjectFailure *failure)
{
    const VcoVitaMemory *native = e->data.transport.context;
    return failure->result == VCO_ACCESS_SESSION &&
           (failure->stage == VCO_PATCH_STAGE_AUTHORITY || failure->stage == VCO_PATCH_STAGE_LOCK ||
            failure->stage == VCO_PATCH_STAGE_INSPECT || failure->stage == VCO_PATCH_STAGE_READ) &&
           !e->blocked && !e->poisoned && !e->retired && native && !native->poisoned &&
           !native->locked;
}
static int inject_admitted(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                           size_t size, int32_t *handle, InjectFailure *failure)
{
    VcoVitaExecution *e = context;
    if (!bytes || !handle || !e->support || !e->support->ready || e->sequence == INT32_MAX ||
        (size != 1 && size != 2 && size != 4) || address < 0x40000000u ||
        (uint64_t)address + size > UINT64_C(0x100000000))
        return failed_at(failure, VCO_PATCH_STAGE_ARGUMENT, -1);
    /* A pre-acquisition authority denial is retryable. Once native ownership
     * is possible, errors keep the existing patch/quarantine semantics. */
    if (!alive(e, token))
        return VCO_ACCESS_SESSION;
    VcoVitaPatch *slot = NULL;
    for (unsigned i = 0; i < VCO_VITA_PATCH_LIMIT; ++i) {
        VcoVitaPatch *p = &e->patches[i];
        if (!p->active && !p->quarantined && !p->referenced && !p->block_referenced) {
            if (!slot)
                slot = p;
            continue;
        }
        if ((uint64_t)address < (uint64_t)p->address + p->size &&
            (uint64_t)p->address < (uint64_t)address + size)
            return failed_at(failure, VCO_PATCH_STAGE_OVERLAP, -1);
    }
    if (!slot)
        return failed_at(failure, VCO_PATCH_STAGE_CAPACITY, -1);
    uint64_t module_generation = 0;
    if (failed_at(failure, VCO_PATCH_STAGE_MODULE_CAPTURE,
                  guard_module(e, token, address, (uint32_t)size, &module_generation, true)))
        return -1;
    /* The module query can outlast the current foreground receipt. No range,
     * block reference or taiHEN handle exists yet: preserve SESSION rather
     * than reporting this clean denial as a permanent injection failure. */
    if (failed_at(failure, VCO_PATCH_STAGE_AUTHORITY, alive(e, token) ? 0 : VCO_ACCESS_SESSION))
        return VCO_ACCESS_SESSION;
    VcoMemoryTransport *t = &e->data.transport;
    const VcoIdentity *id = &e->data.session->identity;
    if (failed_at(failure, VCO_PATCH_STAGE_LOCK, t->lock(t->context, id, address, size)))
        return clean_preinject_denial(e, failure) ? VCO_ACCESS_SESSION : -1;
    VcoMemoryRegion region = {0};
    int32_t native_handle = -1;
    uint8_t readable[4];
    if (!failed_at(failure, VCO_PATCH_STAGE_INSPECT,
                   t->inspect(t->context, id, address, size, &region)) &&
        !failed_at(failure, VCO_PATCH_STAGE_AUTHORITY, alive(e, token) ? 0 : VCO_ACCESS_SESSION) &&
        !failed_at(failure, VCO_PATCH_STAGE_PERMISSION,
                   (region.permissions & VCO_MEM_READ) ? 0 : -1) &&
        !failed_at(failure, VCO_PATCH_STAGE_READ,
                   t->read(t->context, id, address, readable, size)) &&
        !failed_at(failure, VCO_PATCH_STAGE_AUTHORITY, alive(e, token) ? 0 : VCO_ACCESS_SESSION)) {
        /* Check readability through the SDK adapter's propagated result. The
         * pinned upstream taiHEN save helper discards its user-copy result.
         * This preflight rejects observed read failure; it does not replace
         * taiHEN's saved bytes or make the later save atomic with this read. */
        VcoVitaMemory *native = t->context;
        SceObjectBase *block_object = NULL;
        int block_reference = -1;
        if (native->locked && native->block >= 0) {
            block_reference = ksceGUIDReferObject(native->block, &block_object);
            trace(e->support, VCO_PATCH_TRACE_BLOCK_REFERENCE, block_reference,
                  (uint32_t)native->block, (uint32_t)size);
        }
        if (block_reference >= 0) {
            *slot = (VcoVitaPatch){.key = ++e->sequence,
                                   .tai_handle = -1,
                                   .memblock = native->block,
                                   .address = address,
                                   .size = (uint32_t)size,
                                   .session = token,
                                   .module_generation = module_generation,
                                   .block_object = block_object,
                                   .block_referenced = true};
            if (!failed_at(failure, VCO_PATCH_STAGE_BLOCK_REFERENCE, block_object ? 0 : -1) &&
                !failed_at(failure, VCO_PATCH_STAGE_BLOCK_MAPPING, same_block(e, slot)) &&
                !failed_at(
                    failure, VCO_PATCH_STAGE_MODULE_INJECT,
                    guard_module(e, token, address, (uint32_t)size, &module_generation, false)) &&
                !failed_at(failure, VCO_PATCH_STAGE_AUTHORITY,
                           alive(e, token) ? 0 : VCO_ACCESS_SESSION)) {
                native_handle =
                    taiInjectAbsForKernel(id->pid, (void *)(uintptr_t)address, bytes, size);
                trace(e->support, VCO_PATCH_TRACE_INJECT, native_handle, (uint32_t)slot->key,
                      (uint32_t)size);
                if (native_handle < 0)
                    (void)failed_at(failure, VCO_PATCH_STAGE_TAI_INJECT, native_handle);
            }
        } else
            (void)failed_at(failure, VCO_PATCH_STAGE_BLOCK_REFERENCE, block_reference);
        if (native_handle >= 0) {
            /* Record ownership before any subsequent failure can lose the handle. */
            slot->tai_handle = native_handle;
            slot->active = true;
            SceObjectBase *object = NULL;
            int result = ksceGUIDReferObject(native_handle, &object);
            trace(e->support, VCO_PATCH_TRACE_PATCH_REFERENCE, result, (uint32_t)native_handle,
                  (uint32_t)slot->key);
            if (result >= 0) {
                slot->object = object;
                slot->referenced = true;
            }
            if (failed_at(failure, VCO_PATCH_STAGE_PATCH_REFERENCE,
                          result < 0 ? result
                          : !object  ? -1
                                     : 0))
                poison(e, slot);
            else if (failed_at(failure, VCO_PATCH_STAGE_BLOCK_MAPPING, same_block(e, slot)) ||
                     failed_at(failure, VCO_PATCH_STAGE_MODULE_CONFIRM,
                               guard_module(e, token, address, (uint32_t)size, &module_generation,
                                            false)) ||
                     failed_at(failure, VCO_PATCH_STAGE_UID_CONFIRM, native_state(e, slot)))
                reject_injected_owner(e, slot);
        } else if (slot->block_referenced && !e->poisoned) {
            if (!drop_block_reference(e, slot))
                memset(slot, 0, sizeof(*slot));
        }
    }
    int unlock_result = t->unlock(t->context, id, address, size);
    trace(e->support, VCO_PATCH_TRACE_INJECT_UNLOCK, unlock_result, (uint32_t)slot->key,
          (uint32_t)size);
    if (unlock_result) {
        (void)failed_at(failure, VCO_PATCH_STAGE_UNLOCK, unlock_result);
        if (!e->blocked)
            trace(e->support, VCO_PATCH_TRACE_BLOCKED, unlock_result, (uint32_t)slot->key,
                  VCO_PATCH_TRACE_REASON_INJECT_UNLOCK);
        e->blocked = true;
    }
    if (native_handle < 0) {
        /* Before taiHEN, a denied read/inspection may still hold a range or
         * block reference. SESSION is safe only after successful unlock and
         * release, with no private ownership remaining. Native copy, mapping,
         * reference and unlock errors keep their permanent failure semantics. */
        return !unlock_result && !slot->active && !slot->quarantined && !slot->referenced &&
                       !slot->block_referenced && !slot->object && !slot->block_object &&
                       clean_preinject_denial(e, failure)
                   ? VCO_ACCESS_SESSION
                   : -1;
    }
    if (slot->quarantined || e->poisoned)
        return -1;
    /* The range unlock is another native boundary. Validate the same receipt
     * again before publishing, including zero (a sampled non-module range),
     * then recheck the original block mapping/object after the native queries.
     * These observations detect changes; they are not a module lifetime pin. */
    if (!unlock_result &&
        (failed_at(failure, VCO_PATCH_STAGE_MODULE_PUBLICATION,
                   guard_module(e, token, address, (uint32_t)size, &module_generation, false)) ||
         failed_at(failure, VCO_PATCH_STAGE_UID_PUBLICATION, native_state(e, slot)) ||
         failed_at(failure, VCO_PATCH_STAGE_BLOCK_PUBLICATION, same_block(e, slot)))) {
        reject_injected_owner(e, slot);
        return -1;
    }
    if (unlock_result)
        return -1;
    if (!alive(e, token)) {
        /* The engine has no receipt for this injection, so its normal pause/OFF
         * cannot release it. Continue the admitted operation with the existing
         * background restore checks: same process, original module stamp,
         * memblock/object and patch UID. Foreground loss grants no new writes.
         * Failed/ambiguous cleanup retains quarantine and is never retried. */
        int cleaned = restore_admitted(e, slot);
        trace(e->support, VCO_PATCH_TRACE_UNPUBLISHED_CLEANUP, cleaned, (uint32_t)slot->tai_handle,
              (uint32_t)slot->key);
        if (failed_at(failure, VCO_PATCH_STAGE_PRIVATE_CLEANUP, cleaned))
            return -1;
        /* No public receipt ever escaped; only after both references were
         * successfully dropped may the private slot become idle again. Keep
         * the executor's monotonic sequence and existing foreground lease. */
        memset(slot, 0, sizeof(*slot));
        return VCO_ACCESS_SESSION;
    }
    *handle = slot->key;
    return 0;
}
static int restore_admitted(VcoVitaExecution *e, VcoVitaPatch *p)
{
    VcoMemoryTransport *t = &e->data.transport;
    const VcoSession *s = e->data.session;
    /* Background restore is allowed; a new/absent process instance is not. */
    /* Holding the original GUID object prevents a release from trusting only
     * the numeric UID. A live replacement is always a permanent ambiguity. */
    int state = native_state(e, p);
    if (state != 0 || e->retired || !same_identity(&e->identity, &s->identity) ||
        !vco_session_matches(s, p->session, false) ||
        t->validate(t->context, &s->identity, false) ||
        guard_module(e, p->session, p->address, p->size, &p->module_generation, false)) {
        p->active = false;
        quarantine(e, p, VCO_PATCH_TRACE_REASON_RESTORE_GUARD);
        e->blocked = true;
        return -1;
    }
    p->active = false; /* Every handle gets at most one release attempt. */
    int result = -1;
    int locked = ksceKernelLockRangeProc(e->identity.pid, (void *)(uintptr_t)p->address, p->size);
    trace(e->support, VCO_PATCH_TRACE_RESTORE_LOCK, locked, (uint32_t)p->key, p->size);
    if (locked >= 0) {
        /* This pins the referenced memblocks according to VitaSDK. It is not a
         * promise that an entire module or suballocation cannot be reused. */
        if (!same_block(e, p) && !t->validate(t->context, &s->identity, false) &&
            !guard_module(e, p->session, p->address, p->size, &p->module_generation, false) &&
            !native_state(e, p)) {
            /* Range acquisition, identity and module queries are native
             * boundaries after the first UID observation. Reconfirm the
             * original patch object before passing its numeric UID to taiHEN.
             * This detects observed closure/replacement; it is not an atomic
             * compare-and-release or a module lifetime lock. */
            result = taiInjectReleaseForKernel(p->tai_handle);
            trace(e->support, VCO_PATCH_TRACE_NATIVE_RELEASE, result, (uint32_t)p->tai_handle,
                  (uint32_t)p->key);
        }
        int unlocked =
            ksceKernelUnlockRangeProc(e->identity.pid, (void *)(uintptr_t)p->address, p->size);
        trace(e->support, VCO_PATCH_TRACE_RESTORE_UNLOCK, unlocked, (uint32_t)p->key, p->size);
        if (unlocked < 0)
            return poison(e, p);
    }
    if (result < 0 || e->poisoned) {
        quarantine(e, p, VCO_PATCH_TRACE_REASON_RESTORE_FAILED);
        e->blocked = true;
        return -1;
    }
    return drop_reference(e, p) || drop_block_reference(e, p) ? -1 : 0;
}
/* Keep the ticket outside all native-operation error exits. Leaving happens
 * only after the helper has recorded every acquired resource and completed its
 * unlock/reference handoff. A closed gate never authorizes cleanup itself. */
static int inject(void *context, uint64_t token, uint32_t address, const uint8_t *bytes,
                  size_t size, int32_t *handle)
{
    VcoVitaExecution *e = context;
    if (!e)
        return -1;
    if (e->admission && !vco_admission_enter(e->admission)) {
        trace(e->support, VCO_PATCH_TRACE_INJECT_FAILURE, -1, address,
              VCO_PATCH_STAGE_ADMISSION << 8 | ((uint32_t)size & 255u));
        return -1;
    }
    InjectFailure failure = {0};
    int result = inject_admitted(context, token, address, bytes, size, handle, &failure);
    if (e->admission)
        (void)vco_admission_leave(e->admission);
    if (result && result != VCO_ACCESS_SESSION)
        trace(e->support, VCO_PATCH_TRACE_INJECT_FAILURE, failure.stage ? failure.result : result,
              address, failure.stage << 8 | ((uint32_t)size & 255u));
    return result;
}
static int restore(VcoVitaExecution *e, VcoVitaPatch *p)
{
    if (e->admission && !vco_admission_enter(e->admission)) {
        /* The patch is still owned and unchanged. Surface the failed cleanup
         * without discarding its receipt or retrying native release. */
        e->blocked = true;
        return -1;
    }
    int result = restore_admitted(e, p);
    if (e->admission)
        (void)vco_admission_leave(e->admission);
    return result;
}
static int release(void *context, uint64_t token, int32_t key)
{
    VcoVitaExecution *e = context;
    for (unsigned i = 0; i < VCO_VITA_PATCH_LIMIT; ++i) {
        VcoVitaPatch *p = &e->patches[i];
        if (p->active && p->key == key && p->session == token)
            return restore(e, p);
    }
    return -1;
}
void vco_vita_execution_init(VcoVitaExecution *e, VcoVitaMemory *m, const VcoSession *s,
                             const VcoVitaPatchSupport *support)
{
    memset(e, 0, sizeof(*e));
    e->data = (VcoMemoryAdapter){s, vco_vita_memory_transport(m)};
    e->identity = s->identity;
    e->support = support;
}
int vco_vita_execution_set_admission(VcoVitaExecution *e, VcoAdmission *gate)
{
    if (!e || e->sealed || e->blocked)
        return -1;
    e->admission = gate;
    return 0;
}
int vco_vita_execution_set_features(VcoVitaExecution *e, VcoVitaExecutionFeatures features)
{
    if (!e || e->sealed || e->blocked)
        return -1;
    e->features = features;
    return 0;
}
VcoCheatMemory vco_vita_execution_callbacks(VcoVitaExecution *e)
{
    VcoCheatMemory memory = {0};
    if (!e)
        return memory;
    e->sealed = true;
    memory.context = e;
    memory.alive = alive;
    memory.read = read_data;
    memory.write = write_data;
    if (e->support && e->support->ready) {
        memory.inject = inject;
        memory.release = release;
    }
    memory.module = e->features.module ? resolve_module : NULL;
    memory.pad = e->features.pad ? sample_pad : NULL;
    return memory;
}
int vco_vita_execution_dispose(VcoVitaExecution *e)
{
    if (!e || e->poisoned)
        return -1;
    if (e->retired)
        return vco_vita_execution_reap_retired(e, &e->identity);
    int result = 0;
    for (unsigned i = 0; i < VCO_VITA_PATCH_LIMIT; ++i) {
        VcoVitaPatch *p = &e->patches[i];
        if (p->active && restore(e, p))
            result = -1;
        if (p->quarantined || p->referenced || p->block_referenced)
            result = -1;
    }
    return result;
}
int vco_vita_execution_rearm(VcoVitaExecution *e)
{
    if (!e || e->poisoned || e->retired || !e->data.session ||
        (e->admission && vco_admission_closed(e->admission)))
        return -1;
    VcoVitaMemory *native = e->data.transport.context;
    if (!native || !native->bound || native->poisoned || native->locked ||
        !same_identity(&e->identity, &native->identity) ||
        !same_identity(&e->identity, &e->data.session->identity) ||
        !vco_session_matches(e->data.session, e->data.session->token, false))
        return -1;
    for (unsigned i = 0; i < VCO_VITA_PATCH_LIMIT; ++i) {
        const VcoVitaPatch *p = &e->patches[i];
        if (p->active || p->quarantined || p->referenced || p->block_referenced || p->object ||
            p->block_object)
            return -1;
    }
    /* A post-injection lease revocation may leave a private handle which
     * dispose later restored. Only that fully cleaned state can reopen. No
     * memset/init: old native/public keys must never be reused in this binding. */
    e->blocked = false;
    return 0;
}
int vco_vita_execution_reap_retired(VcoVitaExecution *e, const VcoIdentity *identity)
{
    if (!e || !identity || !vco_identity_valid(identity) ||
        !same_identity(&e->identity, identity) || e->poisoned)
        return -1;
    e->retired = e->blocked = true;
    int pending = 0;
    for (unsigned i = 0; i < VCO_VITA_PATCH_LIMIT; ++i) {
        VcoVitaPatch *p = &e->patches[i];
        if (!p->active && !p->quarantined && !p->referenced && !p->block_referenced)
            continue;
        p->active = false;
        quarantine(e, p, VCO_PATCH_TRACE_REASON_RETIRED);
        int state = native_state(e, p);
        if (state < 0)
            return -1;
        if (state == 0) {
            pending = 1;
            continue;
        }
        if (drop_reference(e, p) || retire_block_reference(e, p))
            return -1;
        p->quarantined = false;
        trace(e->support, VCO_PATCH_TRACE_RETIRED_CLEARED, 0, (uint32_t)p->tai_handle,
              (uint32_t)p->key);
    }
    return pending;
}
