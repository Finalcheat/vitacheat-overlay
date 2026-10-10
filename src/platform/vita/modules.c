#include "modules.h"
#include <psp2kern/kernel/modulemgr.h>
#include <taihen.h>
#include <string.h>

int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *address);
typedef int (*ModuleListFn)(SceUID, int, int, SceUID *, SceSize *);
typedef int (*ModuleInfoFn)(SceUID, SceUID, SceKernelModuleInfo *);
typedef SceUID (*MainModuleFn)(SceUID);

static int fail(VcoVitaModules *p, uint32_t event, int32_t result, uint32_t subject,
                uint32_t detail)
{
    p->failure = (VcoVitaModuleFailure){event, subject, detail, result};
    if (p->trace)
        p->trace(p->trace_context, event, result, subject, detail);
    return -1;
}

static uintptr_t resolve_export(const uint32_t functions[2])
{
    const uint32_t libraries[] = {0xC445FA63, 0x92C9FFC2};
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t address = 0;
        if (module_get_export_func(KERNEL_PID, "SceKernelModulemgr", libraries[i], functions[i],
                                   &address) >= 0 &&
            address)
            return address;
    }
    return 0;
}

void vco_vita_modules_invalidate(VcoVitaModules *p)
{
    if (!p)
        return;
    p->session = p->generation = 0;
    p->count = p->mapping_count = 0;
    p->main_uid = -1;
    p->mapped = false;
    memset(&p->identity, 0, sizeof(p->identity));
}

int vco_vita_modules_init(VcoVitaModules *p, const VcoMemoryAdapter *data,
                          VcoVitaModuleRecord *storage, unsigned capacity)
{
    if (!p)
        return -1;
    memset(p, 0, sizeof(*p));
    p->main_uid = -1;
    if (!data || !data->session || !data->transport.validate || !storage || !capacity ||
        capacity > VCO_VITA_MODULE_LIMIT)
        return fail(p, VCO_MODULE_TRACE_INIT, -1, 0, 0);
    p->data = data;
    p->records = storage;
    p->capacity = capacity;
    const uint32_t list_nids[] = {0x97CF7B4E, 0xB72C75A4};
    const uint32_t info_nids[] = {0xD269F915, 0xDAA90093};
    const uint32_t main_nids[] = {0x20A27FA9, 0x679F5144};
    p->list_function = resolve_export(list_nids);
    p->info_function = resolve_export(info_nids);
    p->main_function = resolve_export(main_nids);
    return p->list_function && p->info_function
               ? 0
               : fail(p, VCO_MODULE_TRACE_INIT, -1, 0,
                      (p->list_function ? 1u : 0u) | (p->info_function ? 2u : 0u) |
                          (p->main_function ? 4u : 0u));
}

static bool same_identity(const VcoIdentity *a, const VcoIdentity *b)
{
    return a->pid == b->pid && a->instance == b->instance &&
           !memcmp(a->title_id, b->title_id, sizeof(a->title_id));
}

static int authority(const VcoVitaModules *p, uint64_t token, const VcoIdentity *expected,
                     bool foreground)
{
    if (!p || !p->data || !p->data->transport.validate)
        return -1;
    if (!vco_session_matches(p->data->session, token, foreground))
        return VCO_ACCESS_SESSION;
    VcoIdentity identity = p->data->session->identity;
    if (!vco_identity_valid(&identity) || (expected && !same_identity(&identity, expected)))
        return VCO_ACCESS_SESSION;
    int result = p->data->transport.validate(p->data->transport.context, &identity, foreground);
    if (result)
        return result;
    return vco_session_matches(p->data->session, token, foreground) &&
                   same_identity(&identity, &p->data->session->identity)
               ? 0
               : VCO_ACCESS_SESSION;
}

static int enumerate(VcoVitaModules *p, uint64_t token, const VcoIdentity *identity, SceUID *uids,
                     unsigned *out_count)
{
    if (!p->list_function)
        return fail(p, VCO_MODULE_TRACE_INIT, -1, (uint32_t)identity->pid, 0);
    int checked = authority(p, token, identity, false);
    if (checked)
        return fail(p, VCO_MODULE_TRACE_AUTHORITY, checked, (uint32_t)identity->pid, 0);
    /* One extra slot distinguishes exact capacity from SDK truncation. */
    SceSize count = p->capacity + 1;
    /* Original VitaCheat Z06 queries this class mask and then reverses the raw
     * list for B indices. Do not sort or replace it with a broader mask. */
    int result = ((ModuleListFn)p->list_function)(identity->pid, 0xff, 1, uids, &count);
    if (result < 0)
        return fail(p, VCO_MODULE_TRACE_ENUMERATE, result, (uint32_t)identity->pid, count);
    if (!count || count > p->capacity)
        return fail(p, VCO_MODULE_TRACE_COUNT, -1, (uint32_t)identity->pid, count);
    checked = authority(p, token, identity, false);
    if (checked)
        return fail(p, VCO_MODULE_TRACE_AUTHORITY, checked, (uint32_t)identity->pid, 1);
    for (unsigned i = 0; i < count; ++i) {
        SceUID uid = uids[i];
        if (uid <= 0 || uid == KERNEL_PID)
            return fail(p, VCO_MODULE_TRACE_COUNT, -1, (uint32_t)uid, i);
        for (unsigned j = 0; j < i; ++j)
            if (uids[j] == uid)
                return fail(p, VCO_MODULE_TRACE_COUNT, -1, (uint32_t)uid, i);
    }
    *out_count = count;
    return 0;
}

static void sort_uids(SceUID *uids, unsigned count)
{
    for (unsigned i = 1; i < count; ++i) {
        SceUID uid = uids[i];
        unsigned j = i;
        while (j && uids[j - 1] > uid) {
            uids[j] = uids[j - 1];
            --j;
        }
        uids[j] = uid;
    }
}

static bool copy_string(char *out, const char *in, unsigned capacity)
{
    const char *end = memchr(in, 0, capacity);
    if (!end)
        return false;
    memcpy(out, in, (size_t)(end - in));
    return true;
}

static void trace_segment(const VcoVitaModules *p, uint32_t uid, unsigned segment, uint32_t base,
                          uint32_t size, uint32_t permissions)
{
    if (!p->trace)
        return;
    p->trace(p->trace_context, VCO_MODULE_TRACE_SEGMENT_BASE, (int32_t)base, uid, segment);
    p->trace(p->trace_context, VCO_MODULE_TRACE_SEGMENT_SIZE, (int32_t)size, uid, segment);
    p->trace(p->trace_context, VCO_MODULE_TRACE_SEGMENT_PERMISSIONS, (int32_t)permissions, uid,
             segment);
}

static int record(VcoVitaModules *p, uint64_t token, const VcoIdentity *identity, SceUID uid,
                  VcoVitaModuleRecord *out)
{
    if (!p->info_function)
        return fail(p, VCO_MODULE_TRACE_INIT, -1, (uint32_t)uid, 0);
    int checked = authority(p, token, identity, false);
    if (checked)
        return fail(p, VCO_MODULE_TRACE_AUTHORITY, checked, (uint32_t)uid, 2);
    SceKernelModuleInfo info = {0};
    info.size = sizeof(info);
    int result = ((ModuleInfoFn)p->info_function)(identity->pid, uid, &info);
    if (result < 0)
        return fail(p, VCO_MODULE_TRACE_INFO, result, (uint32_t)uid, 0);
    checked = authority(p, token, identity, false);
    if (checked)
        return fail(p, VCO_MODULE_TRACE_AUTHORITY, checked, (uint32_t)uid, 3);
    if (info.modid <= 0)
        return fail(p, VCO_MODULE_TRACE_FIELDS, info.modid, (uint32_t)uid, 1);
    VcoVitaModuleRecord candidate = {0};
    candidate.uid = uid;
    /* Captured Modulemgr verifies the input GUID against object+0x14, then
     * returns object+0x18 (PUID) at info+4 for non-KERNEL_PID. Comparing these
     * two namespaces rejects valid user modules and breaks every B code. */
    candidate.process_uid = info.modid;
    candidate.attributes = info.modattr;
    candidate.version = (uint32_t)info.modver[0] | (uint32_t)info.modver[1] << 8;
    candidate.state = info.state;
    if (!info.module_name[0] ||
        !copy_string(candidate.name, info.module_name, sizeof(candidate.name)))
        return fail(p, VCO_MODULE_TRACE_FIELDS, -1, (uint32_t)uid, 2);
    if (!copy_string(candidate.path, info.path, sizeof(candidate.path)))
        return fail(p, VCO_MODULE_TRACE_FIELDS, -1, (uint32_t)uid, 3);
    for (unsigned i = 0; i < VCO_VITA_MODULE_RECORD_SEGMENTS; ++i) {
        uintptr_t base = (uintptr_t)info.segments[i].vaddr;
        uint32_t size = info.segments[i].memsz;
        /* Modulemgr copies vaddr and memsz independently, including an
         * addressed zero-length descriptor. It owns no accessible range, but
         * remains identity metadata. resolve() still rejects an empty B target.
         * Keep the existing lower-bound/overflow checks for nonempty ranges. */
        if (base > UINT32_MAX ||
            (size && (base < 0x40000000u || (uint64_t)base + size > UINT64_C(0x100000000)))) {
            trace_segment(p, (uint32_t)uid, i, (uint32_t)base, size, info.segments[i].perms);
            return fail(p, VCO_MODULE_TRACE_FIELDS, -1, (uint32_t)uid, 4 + i);
        }
        candidate.segments[i] =
            (VcoVitaModuleSegment){(uint32_t)base, size, info.segments[i].perms};
    }
    *out = candidate;
    return 0;
}

static int main_uid(VcoVitaModules *p, uint64_t token, const VcoIdentity *identity, int32_t *out)
{
    if (!p->main_function) {
        *out = -1;
        return 0;
    }
    int checked = authority(p, token, identity, false);
    if (checked)
        return fail(p, VCO_MODULE_TRACE_AUTHORITY, checked, (uint32_t)identity->pid, 4);
    SceUID uid = ((MainModuleFn)p->main_function)(identity->pid);
    if (uid <= 0 || uid == KERNEL_PID)
        return fail(p, VCO_MODULE_TRACE_MAIN, uid, (uint32_t)identity->pid, 0);
    checked = authority(p, token, identity, false);
    if (checked)
        return fail(p, VCO_MODULE_TRACE_AUTHORITY, checked, (uint32_t)identity->pid, 5);
    *out = uid;
    return 0;
}

static int verify_list(VcoVitaModules *p, uint64_t token)
{
    SceUID uids[VCO_VITA_MODULE_LIMIT + 1];
    unsigned count = 0;
    if (enumerate(p, token, &p->identity, uids, &count))
        return -1;
    if (count != p->count)
        return fail(p, VCO_MODULE_TRACE_LIST_CHANGED, -1, (uint32_t)p->identity.pid, count);
    for (unsigned i = 0; i < count; ++i)
        if (uids[i] != p->enumeration[i])
            return fail(p, VCO_MODULE_TRACE_LIST_CHANGED, -1, (uint32_t)uids[i], i);
    int32_t main = -1;
    if (main_uid(p, token, &p->identity, &main))
        return -1;
    return main == p->main_uid ? 0 : fail(p, VCO_MODULE_TRACE_MAIN, -1, (uint32_t)main, 1);
}

static int verify_records(VcoVitaModules *p, uint64_t token, const uint16_t *indices,
                          unsigned count)
{
    if (token != p->session || verify_list(p, token))
        return -1;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < count; ++i) {
            unsigned index = indices ? indices[i] : i;
            if (index == UINT16_MAX)
                continue;
            if (index >= p->count)
                return -1;
            VcoVitaModuleRecord current;
            if (record(p, token, &p->identity, p->records[index].uid, &current))
                return -1;
            if (memcmp(&current, &p->records[index], sizeof(current)))
                return fail(p, VCO_MODULE_TRACE_RECORD_CHANGED, -1, (uint32_t)p->records[index].uid,
                            index);
        }
    }
    return verify_list(p, token) || authority(p, token, &p->identity, false) ? -1 : 0;
}

int vco_vita_modules_snapshot(VcoVitaModules *p, uint64_t token, uint64_t *generation,
                              unsigned *count)
{
    if (!p)
        return -1;
    memset(&p->failure, 0, sizeof(p->failure));
    vco_vita_modules_invalidate(p);
    if (!generation || !count || !p->records || !p->capacity || !p->list_function ||
        !p->info_function || p->sequence == UINT64_MAX)
        return fail(p, VCO_MODULE_TRACE_INIT, -1, 0,
                    (p->list_function ? 1u : 0u) | (p->info_function ? 2u : 0u) |
                        (p->main_function ? 4u : 0u));
    int checked = authority(p, token, NULL, false);
    if (checked)
        return fail(p, VCO_MODULE_TRACE_AUTHORITY, checked, 0, 6);
    p->identity = p->data->session->identity;
    p->session = token;
    SceUID uids[VCO_VITA_MODULE_LIMIT + 1];
    if (enumerate(p, token, &p->identity, uids, &p->count) ||
        main_uid(p, token, &p->identity, &p->main_uid))
        goto fail;
    memcpy(p->enumeration, uids, p->count * sizeof(uids[0]));
    sort_uids(uids, p->count); /* Canonical diagnostic records, separate from B order. */
    bool main_found = p->main_uid == -1;
    bool empty_traced = false;
    for (unsigned i = 0; i < p->count; ++i) {
        if (record(p, token, &p->identity, uids[i], &p->records[i]))
            goto fail;
        /* One addressed empty descriptor per explicit snapshot is enough to
         * confirm its raw shape. Regular verification/resolve stays silent. */
        for (unsigned j = 0; p->trace && !empty_traced && j < VCO_VITA_MODULE_RECORD_SEGMENTS;
             ++j) {
            const VcoVitaModuleSegment *segment = &p->records[i].segments[j];
            if (segment->base && !segment->size) {
                trace_segment(p, (uint32_t)uids[i], j, segment->base, 0, segment->permissions);
                p->trace(p->trace_context, VCO_MODULE_TRACE_EMPTY, 0, (uint32_t)uids[i], j);
                empty_traced = true;
            }
        }
        main_found |= uids[i] == p->main_uid;
    }
    if (!main_found) {
        (void)fail(p, VCO_MODULE_TRACE_MAIN, -1, (uint32_t)p->main_uid, 2);
        goto fail;
    }
    if (verify_records(p, token, NULL, p->count))
        goto fail;
    p->generation = ++p->sequence;
    *generation = p->generation;
    *count = p->count;
    if (p->trace)
        p->trace(p->trace_context, VCO_MODULE_TRACE_SNAPSHOT, 0, (uint32_t)p->identity.pid,
                 p->count);
    return 0;
fail:
    vco_vita_modules_invalidate(p);
    return -1;
}

int vco_vita_modules_confirm_mapping(VcoVitaModules *p, uint64_t token, uint64_t generation,
                                     const int32_t *uids, unsigned count)
{
    if (!p)
        return -1;
    p->mapped = false;
    p->mapping_count = 0;
    if (!uids || !count || count > VCO_VITA_MODULE_LIMIT || !generation ||
        generation != p->generation || !token || token != p->session)
        return fail(p, VCO_MODULE_TRACE_MAPPING, -1, 0, count);
    uint16_t mapping[VCO_VITA_MODULE_LIMIT];
    bool any = false;
    for (unsigned i = 0; i < count; ++i) {
        mapping[i] = UINT16_MAX;
        if (!uids[i])
            continue;
        for (unsigned j = 0; j < i; ++j)
            if (uids[j] == uids[i])
                return fail(p, VCO_MODULE_TRACE_MAPPING, -1, (uint32_t)uids[i], i);
        for (unsigned j = 0; j < p->count; ++j) {
            if (p->records[j].uid == uids[i]) {
                mapping[i] = (uint16_t)j;
                any = true;
                break;
            }
        }
        if (mapping[i] == UINT16_MAX)
            return fail(p, VCO_MODULE_TRACE_MAPPING, -1, (uint32_t)uids[i], i);
    }
    if (!any)
        return fail(p, VCO_MODULE_TRACE_MAPPING, -1, 0, count);
    if (verify_records(p, token, mapping, count)) {
        vco_vita_modules_invalidate(p);
        return -1;
    }
    memcpy(p->mapping, mapping, count * sizeof(mapping[0]));
    p->mapping_count = count;
    p->mapped = true;
    return 0;
}

int vco_vita_modules_map_z06(VcoVitaModules *p, uint64_t token, uint64_t generation)
{
    if (!p)
        return -1;
    p->mapped = false;
    p->mapping_count = 0;
    if (!generation || generation != p->generation || !token || token != p->session || !p->count ||
        p->count > VCO_VITA_MODULE_LIMIT)
        return fail(p, VCO_MODULE_TRACE_MAPPING, -1, 0, p->count);
    int32_t uids[VCO_VITA_MODULE_LIMIT];
    for (unsigned i = 0; i < p->count; ++i)
        uids[i] = p->enumeration[p->count - 1 - i];
    return vco_vita_modules_confirm_mapping(p, token, generation, uids, p->count);
}

void vco_vita_modules_prepare_idle(void *context, uint64_t token)
{
    VcoVitaModules *p = context;
    uint64_t generation;
    unsigned count;
    if (!vco_vita_modules_snapshot(p, token, &generation, &count))
        (void)vco_vita_modules_map_z06(p, token, generation);
}

int vco_vita_modules_resolve(void *context, uint64_t token, unsigned index, unsigned segment,
                             uint32_t *base, uint32_t *size)
{
    VcoVitaModules *p = context;
    if (!p || !base || !size || base == size || !p->mapped || !p->generation ||
        index >= p->mapping_count || index >= VCO_VITA_MODULE_LIMIT ||
        segment >= VCO_VITA_MODULE_SEGMENTS || p->mapping[index] == UINT16_MAX) {
        if (p && p->trace) {
            /* Replay the original SDK/validation failure at the B execution
             * request. Boot/load diagnostics may have aged out of the ring. */
            if (p->failure.event)
                p->trace(p->trace_context, p->failure.event, p->failure.result, p->failure.subject,
                         p->failure.detail);
            p->trace(p->trace_context, VCO_MODULE_TRACE_UNAVAILABLE,
                     p->failure.event ? p->failure.result : -1, index, p->failure.event);
        }
        return -1;
    }
    uint16_t selected = p->mapping[index];
    int checked = authority(p, token, &p->identity, true);
    if (checked) {
        /* Foreground leases do not own the persistent module mapping. Keep it
         * only if the retained process identity can still be validated. */
        if (checked == VCO_ACCESS_SESSION && !authority(p, token, &p->identity, false))
            return VCO_ACCESS_SESSION;
        vco_vita_modules_invalidate(p);
        return -1;
    }
    if (verify_records(p, token, &selected, 1)) {
        vco_vita_modules_invalidate(p);
        return -1;
    }
    checked = authority(p, token, &p->identity, true);
    if (checked) {
        if (checked == VCO_ACCESS_SESSION && !authority(p, token, &p->identity, false))
            return VCO_ACCESS_SESSION;
        vco_vita_modules_invalidate(p);
        return -1;
    }
    const VcoVitaModuleRecord *record = &p->records[selected];
    if (record->state != SCE_KERNEL_MODULE_STATE_STARTED || !record->segments[segment].size)
        return fail(p, VCO_MODULE_TRACE_RANGE, -1, (uint32_t)record->uid,
                    record->state << 8 | segment);
    *base = record->segments[segment].base;
    *size = record->segments[segment].size;
    return 0;
}

int vco_vita_modules_guard(void *context, uint64_t token, uint32_t address, uint32_t size,
                           uint64_t *generation)
{
    VcoVitaModules *p = context;
    if (!p || !generation || !size || address < 0x40000000u ||
        (uint64_t)address + size > UINT64_C(0x100000000) || !p->mapped || !p->generation ||
        (*generation && *generation != p->generation))
        return -1;
    uint16_t selected = UINT16_MAX;
    for (unsigned i = 0; i < p->count; ++i) {
        const VcoVitaModuleRecord *r = &p->records[i];
        for (unsigned segment = 0; segment < VCO_VITA_MODULE_RECORD_SEGMENTS; ++segment) {
            const VcoVitaModuleSegment *s = &r->segments[segment];
            if (!s->size || (uint64_t)address >= (uint64_t)s->base + s->size ||
                (uint64_t)s->base >= (uint64_t)address + size)
                continue;
            if (selected != UINT16_MAX || address < s->base ||
                (uint64_t)address + size > (uint64_t)s->base + s->size)
                return -1; /* Partial/overlapping segment ownership is ambiguous. */
            selected = (uint16_t)i;
        }
    }
    if ((*generation && selected == UINT16_MAX) ||
        verify_records(p, token, selected == UINT16_MAX ? NULL : &selected,
                       selected == UINT16_MAX ? p->count : 1)) {
        vco_vita_modules_invalidate(p);
        return -1;
    }
    if (selected != UINT16_MAX && p->records[selected].state != SCE_KERNEL_MODULE_STATE_STARTED)
        return -1;
    *generation = selected == UINT16_MAX ? 0 : p->generation;
    return 0;
}
