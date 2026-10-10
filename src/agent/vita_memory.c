#include "vita_memory.h"
#include "native_service.h"
#include "user_bridge.h"
#include <taihen.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>
enum { FREE, PUBLISHED, RETAINED, CONSUMED_FAILURE };
#define DIGITAL UINT32_C(0x0000F3F9)
#define PS_BUTTON UINT32_C(0x00010000)
#define SYSTEM UINT32_C(0x40390000)
static bool scalar(uint32_t address, size_t size)
{
    return (size == 1 || size == 2 || size == 4) && address >= 0x40000000u &&
           (uint64_t)address + size <= UINT64_C(0x100000000);
}
static int access_error(int error)
{
    return error == VCO_AGENT_OK ? 0
           : error == VCO_AGENT_BUSY || error == VCO_AGENT_STALE || error == VCO_AGENT_OWNER ||
                   error == VCO_AGENT_CLOSED
               ? VCO_ACCESS_SESSION
               : -1;
}
/* BUSY from a read-only bridge query can mean another client briefly owns
 * the broker gate. Wait only on that exact status, then query the current
 * session again. Never replay a write, injection or native release. */
static bool retry_query(int status, unsigned attempt)
{
    return status == VCO_AGENT_BUSY && attempt < 7u && sceKernelDelayThread(1000u) >= 0;
}
static int query_permission(uint64_t session, bool foreground)
{
    unsigned attempt = 0;
    int status;
    do {
        status = vcoAgentPermission(session, foreground);
    } while (retry_query(status, attempt++));
    return status;
}
static void trace_bounded(VcoGameNativeMemory *m, unsigned event, int result, uint32_t subject,
                          uint32_t detail, unsigned limit)
{
    if (m->diagnostics >= limit)
        return;
    ++m->diagnostics;
    VcoAgentTraceRequest request = {.session = m->binding.session,
                                    .abi = VCO_AGENT_ABI,
                                    .size = sizeof(request),
                                    .event = event,
                                    .result = result,
                                    .subject = subject,
                                    .detail = detail};
    unsigned attempt = 0;
    int status;
    do {
        status = vcoAgentTrace(&request, sizeof(request));
    } while (retry_query(status, attempt++));
}
static void trace(VcoGameNativeMemory *m, unsigned event, int result, uint32_t subject,
                  uint32_t detail)
{
    if (event == VCO_GAME_TRACE_QUERY ||
        (event == VCO_GAME_TRACE_GUARD && !m->restore_cause_event)) {
        m->restore_cause_event = event;
        m->restore_cause_result = result;
        m->restore_cause_subject = subject;
        m->restore_cause_detail = detail;
    }
    /* Repeated foreground denials must not spend the two slots reserved for
     * the first restore failure. Total admission remains sixteen per prepare. */
    trace_bounded(m, event, result, subject, detail, 14u);
}
static void trace_restore_failure(VcoGameNativeMemory *m, unsigned event, int result,
                                  uint32_t subject, uint32_t detail)
{
    if (m->restore_failure_logged)
        return;
    m->restore_failure_logged = true;
    if (m->restore_cause_event)
        trace_bounded(m, m->restore_cause_event, m->restore_cause_result, m->restore_cause_subject,
                      m->restore_cause_detail, 16u);
    trace_bounded(m, event, result, subject, detail, 16u);
}
enum { QUERY_PERMISSION = 1, QUERY_MODULES, QUERY_MODULE_INFO, QUERY_READ };
static int authorize(VcoGameNativeMemory *m, uint64_t session, bool foreground)
{
    if (!m || !m->initialized || session != m->binding.session)
        return VCO_ACCESS_SESSION;
    int status = query_permission(session, foreground);
    if (status)
        trace(m, VCO_GAME_TRACE_QUERY, status, 0, QUERY_PERMISSION << 16 | foreground);
    return access_error(status);
}
static int query_modules(VcoGameNativeMemory *m, VcoGameModuleList *out)
{
    unsigned attempt = 0;
    int status;
    do {
        status = vcoAgentModules(m->binding.session, out, sizeof(*out));
    } while (retry_query(status, attempt++));
    if (status)
        trace(m, VCO_GAME_TRACE_QUERY, status, 0, QUERY_MODULES << 16);
    return access_error(status);
}
static int query_module_info(VcoGameNativeMemory *m, int32_t uid, VcoGameModuleInfo *out)
{
    unsigned attempt = 0;
    int status;
    do {
        status = vco_game_bridge_module_info(m->binding.session, uid, out, sizeof(*out));
    } while (retry_query(status, attempt++));
    if (status)
        trace(m, VCO_GAME_TRACE_QUERY, status, (uint32_t)uid, QUERY_MODULE_INFO << 16);
    return access_error(status);
}
static int query_read(VcoGameNativeMemory *m, uint32_t address, uint8_t *out, uint32_t size)
{
    unsigned attempt = 0;
    int status;
    do {
        status = vco_game_bridge_read(m->binding.session, address, out, size);
    } while (retry_query(status, attempt++));
    if (status)
        trace(m, VCO_GAME_TRACE_QUERY, status, address, QUERY_READ << 16 | size);
    return access_error(status);
}
VcoError vco_game_native_memory_init(VcoGameNativeMemory *m, const VcoAgentBinding *binding)
{
    if (!m || m->initialized || !binding || binding->abi != VCO_AGENT_ABI ||
        binding->size != sizeof(*binding) || binding->reserved)
        return VCO_E_ARGUMENT;
    for (unsigned i = 0; i < sizeof(binding->padding); ++i)
        if (binding->padding[i])
            return VCO_E_ARGUMENT;
    VcoIdentity identity = {.pid = binding->pid, .instance = binding->session};
    memcpy(identity.title_id, binding->title_id, 10);
    if (!vco_identity_valid(&identity))
        return VCO_E_ARGUMENT;
    if (query_permission(binding->session, false) != VCO_AGENT_OK)
        return VCO_E_SESSION;
    memset(m, 0, sizeof(*m));
    m->binding = *binding;
    m->initialized = true;
    return VCO_OK;
}
static int alive(void *context, uint64_t session)
{
    VcoGameNativeMemory *m = context;
    return m && !m->blocked && !authorize(m, session, true);
}
static int read_bytes(void *context, uint64_t session, uint32_t address, uint8_t *out, size_t size)
{
    VcoGameNativeMemory *m = context;
    if (!m || !m->initialized || session != m->binding.session)
        return VCO_ACCESS_SESSION;
    if (m->blocked || !out || !scalar(address, size))
        return -1;
    uint8_t bytes[4];
    int error = query_read(m, address, bytes, (uint32_t)size);
    if (!error)
        memcpy(out, bytes, size);
    return error;
}
static int write_bytes(void *context, uint64_t session, uint32_t address, const uint8_t *input,
                       size_t size)
{
    VcoGameNativeMemory *m = context;
    if (!m || !m->initialized || session != m->binding.session)
        return VCO_ACCESS_SESSION;
    if (m->blocked || !input || !scalar(address, size))
        return -1;
    return access_error(vco_game_bridge_write(session, address, input, (uint32_t)size));
}
static int list_unchanged(VcoGameNativeMemory *m)
{
    VcoGameModuleList list;
    int error = query_modules(m, &list);
    if (error)
        return error;
    return list.count == m->list.count &&
                   !memcmp(list.ids, m->list.ids, list.count * sizeof(list.ids[0]))
               ? 0
               : -1;
}
static int same_module(VcoGameNativeMemory *m, unsigned index)
{
    if (!m->mapped || index >= m->list.count)
        return -1;
    VcoGameModuleInfo info;
    int error = query_module_info(m, m->modules[index].uid, &info);
    if (error) {
        trace(m, VCO_GAME_TRACE_GUARD, error, (uint32_t)m->modules[index].process_uid,
              UINT32_C(0x80000000) | index);
        return error;
    }
    const uint8_t *expected = (const uint8_t *)&m->modules[index];
    const uint8_t *actual = (const uint8_t *)&info;
    for (unsigned i = 0; i < sizeof(info); ++i)
        if (expected[i] != actual[i]) {
            trace(m, VCO_GAME_TRACE_GUARD, (int)expected[i] << 8 | actual[i],
                  (uint32_t)m->modules[index].process_uid, i);
            return -1;
        }
    return 0;
}
void vco_game_native_memory_prepare(void *context, uint64_t session)
{
    VcoGameNativeMemory *m = context;
    if (!m || !m->initialized || session != m->binding.session || m->owned || m->blocked)
        return;
    m->diagnostics = 0;
    m->inject_logged = m->release_logged = false;
    m->restore_failure_logged = false;
    m->restore_cause_event = 0;
    m->mapped = false;
    int error = authorize(m, session, false);
    if (!error)
        error = query_modules(m, &m->list);
    if (!error && m->list.count > VCO_GAME_MODULE_LIMIT)
        error = -1;
    for (unsigned i = 0; !error && i < m->list.count; ++i)
        error = query_module_info(m, m->list.ids[i], &m->modules[i]);
    if (!error)
        error = list_unchanged(m);
    for (unsigned i = 0; !error && i < m->list.count; ++i) {
        VcoGameModuleInfo value;
        error = query_module_info(m, m->list.ids[i], &value);
        if (!error && memcmp(&value, &m->modules[i], sizeof(value)))
            error = -1;
    }
    if (!error)
        error = authorize(m, session, false);
    m->snapshot_error = error;
    m->mapped = error == 0;
}
static int module(void *context, uint64_t session, unsigned index, unsigned segment, uint32_t *base,
                  uint32_t *size)
{
    VcoGameNativeMemory *m = context;
    int error = authorize(m, session, true);
    if (error)
        return error;
    if (!base || !size || base == size || !m->mapped || index >= m->list.count || segment > 1)
        return m->snapshot_error == VCO_ACCESS_SESSION ? VCO_ACCESS_SESSION : -1;
    unsigned raw = m->list.count - 1u - index; /* Exact Z06 B-index order. */
    error = list_unchanged(m);
    if (!error)
        error = same_module(m, raw);
    if (!error)
        error = authorize(m, session, true);
    if (error)
        return error;
    VcoGameModuleSegment value = m->modules[raw].segments[segment];
    if (!value.size)
        return -1;
    *base = value.base;
    *size = value.size;
    return 0;
}
static void disarm(VcoGameNativeMemory *m)
{
    m->released = m->armed = false;
}
static int pad(void *context, uint64_t session, uint32_t *device, uint32_t *buttons)
{
    VcoGameNativeMemory *m = context;
    if (!m || !device || !buttons || device == buttons)
        return -1;
    if (!m->initialized || session != m->binding.session)
        return VCO_ACCESS_SESSION;
    VcoGamePadSample sample;
    int error = access_error(vcoAgentPad(session, &sample, sizeof(sample)));
    if (error) {
        disarm(m);
        return error;
    }
    if (m->pad_known && sample.capture_epoch < m->capture_epoch) {
        disarm(m);
        return -1;
    }
    if (!m->pad_known || sample.capture_epoch != m->capture_epoch || sample.device != m->device)
        disarm(m);
    m->capture_epoch = sample.capture_epoch;
    m->device = sample.device;
    m->pad_known = true;
    if (sample.capturing || (sample.buttons & PS_BUTTON)) {
        disarm(m);
        return VCO_PAD_SUPPRESSED;
    }
    if (sample.buttons & ~(DIGITAL | SYSTEM)) {
        disarm(m);
        return -1;
    }
    uint32_t digital = sample.buttons & DIGITAL;
    if (!m->armed) {
        if (!m->released) {
            if (!digital) {
                m->released = true;
                m->release_stamp = sample.stamp;
            }
            return VCO_PAD_SUPPRESSED;
        }
        /* A fresh neutral sample is valid input: C with a zero button mask
         * must match neutral state, rather than remain suppressed forever. */
        if (sample.stamp <= m->release_stamp)
            return VCO_PAD_SUPPRESSED;
        m->armed = true;
    }
    *device = sample.device;
    *buttons = digital;
    return 0;
}
static int owner_of(VcoGameNativeMemory *m, uint32_t address, size_t width, unsigned *segment)
{
    int owner = -1;
    for (unsigned i = 0; i < m->list.count; ++i)
        for (unsigned j = 0; j < 4; ++j) {
            VcoGameModuleSegment s = m->modules[i].segments[j];
            if (s.size && (uint64_t)address < (uint64_t)s.base + s.size &&
                (uint64_t)s.base < (uint64_t)address + width) {
                if (owner >= 0 || address < s.base ||
                    (uint64_t)address + width > (uint64_t)s.base + s.size)
                    return -2;
                owner = (int)i;
                *segment = j;
            }
        }
    return owner;
}
static int patch_guard(VcoGameNativeMemory *m, const VcoGameNativePatch *p)
{
    int error = authorize(m, m->binding.session, false);
    if (error || !m->mapped)
        return error ? error : -1;
    if (p->owner >= 0)
        return same_module(m, (unsigned)p->owner);
    error = list_unchanged(m);
    for (unsigned i = 0; !error && i < m->list.count; ++i)
        error = same_module(m, i);
    return error;
}
static int release_native(VcoGameNativeMemory *m, VcoGameNativePatch *p)
{
    if (p->state == CONSUMED_FAILURE) {
        m->blocked = true;
        return -1;
    }
    m->restore_cause_event = 0;
    int guard = patch_guard(m, p);
    if (guard) {
        trace_restore_failure(m, VCO_GAME_TRACE_RELEASE_GUARD, guard, (uint32_t)p->native_uid,
                              p->address);
        m->blocked = true;
        return -1;
    }
    /* User taiHEN consumes the UID even on a native failure. Mark before calling
     * so a finalizer cannot retry an ID whose ownership has ended. */
    p->state = CONSUMED_FAILURE;
    int error = taiInjectRelease(p->native_uid);
    if (error < 0)
        trace_restore_failure(m, VCO_GAME_TRACE_RELEASE, error, (uint32_t)p->native_uid,
                              p->address);
    else if (!m->release_logged)
        trace(m, VCO_GAME_TRACE_RELEASE, error, (uint32_t)p->native_uid, p->address);
    m->release_logged = true;
    if (error < 0) {
        m->blocked = true;
        return -1;
    }
    p->state = FREE;
    --m->owned;
    return 0;
}
static int inject(void *context, uint64_t session, uint32_t address, const uint8_t *input,
                  size_t size, int32_t *handle)
{
    VcoGameNativeMemory *m = context;
    int error = authorize(m, session, true);
    if (error)
        return error;
    if (!input || !handle || !scalar(address, size) || m->blocked || !m->mapped ||
        m->sequence == INT32_MAX || m->owned >= VCO_ENGINE_CLAIMS)
        return -1;
    unsigned slot = 0, segment = 0;
    while (slot < VCO_ENGINE_CLAIMS && m->patches[slot].state != FREE)
        ++slot;
    if (slot == VCO_ENGINE_CLAIMS)
        return -1;
    int owner = owner_of(m, address, size, &segment);
    if (owner == -2)
        return -1;
    VcoGameNativePatch candidate = {.key = ++m->sequence,
                                    .address = address,
                                    .owner = (int16_t)owner,
                                    .width = (uint8_t)size,
                                    .state = RETAINED};
    error = patch_guard(m, &candidate);
    uint8_t original[4], bytes[4];
    if (!error)
        error = query_read(m, address, original, (uint32_t)size);
    if (!error)
        error = authorize(m, session, true);
    if (error)
        return error;
    memcpy(bytes, input, size);
    int uid;
    if (owner >= 0) {
        tai_offset_args_t args = {.size = sizeof(args),
                                  .modid = m->modules[owner].process_uid,
                                  .segidx = (int)segment,
                                  .offset = address - m->modules[owner].segments[segment].base,
                                  .source = bytes,
                                  .source_size = size};
        uid = taiInjectDataForUser(&args);
    } else
        uid = taiInjectAbs((void *)(uintptr_t)address, bytes, size);
    if (!m->inject_logged || uid < 0) {
        trace(m, VCO_GAME_TRACE_INJECT, uid, address, (uint32_t)size);
        m->inject_logged = true;
    }
    if (uid < 0)
        return -1;
    candidate.native_uid = uid;
    m->patches[slot] = candidate;
    ++m->owned;
    error = patch_guard(m, &m->patches[slot]);
    if (!error) {
        error = authorize(m, session, true);
        if (error)
            trace(m, VCO_GAME_TRACE_PERMISSION, error, address, (uint32_t)size);
    }
    if (error) {
        /* A private receipt exists even if no runtime handle was published. */
        if (release_native(m, &m->patches[slot])) {
            m->blocked = true;
            return -1;
        }
        return error;
    }
    m->patches[slot].state = PUBLISHED;
    *handle = candidate.key;
    return 0;
}
static int release_patch(void *context, uint64_t session, int32_t handle)
{
    VcoGameNativeMemory *m = context;
    if (!m || !m->initialized || session != m->binding.session || handle <= 0)
        return -1;
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i)
        if (m->patches[i].state == PUBLISHED && m->patches[i].key == handle) {
            m->patches[i].state = RETAINED; /* Runtime consumes its local handle. */
            return release_native(m, &m->patches[i]);
        }
    return -1;
}
VcoError vco_game_native_memory_finalize(void *context)
{
    VcoGameNativeMemory *m = context;
    if (!m || !m->initialized)
        return VCO_E_ARGUMENT;
    VcoError error = VCO_OK;
    for (unsigned i = 0; i < VCO_ENGINE_CLAIMS; ++i)
        if (m->patches[i].state != FREE && release_native(m, &m->patches[i]))
            error = VCO_E_RESTORE;
    if (!error) {
        m->blocked = false;
        m->mapped = false;
    }
    return error;
}
VcoCheatMemory vco_game_native_memory_callbacks(VcoGameNativeMemory *m)
{
    return (VcoCheatMemory){m, alive, read_bytes, write_bytes, module, pad, inject, release_patch};
}
