#include "memory.h"
#include "memblock_lookup.h"
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>
#include <string.h>
int module_get_export_func(SceUID pid, const char *module, uint32_t library, uint32_t function,
                           uintptr_t *address);
static int failure(VcoVitaMemory *m, uint32_t event, int result, uint32_t subject, uint32_t detail)
{
    if (m && m->trace)
        m->trace(m->trace_context, event, result, subject, detail);
    return -1;
}
int vco_vita_memory_set_trace(VcoVitaMemory *m, VcoVitaMemoryTrace callback, void *context)
{
    if (!m || m->bound || m->locked || m->poisoned)
        return -1;
    m->trace = callback;
    m->trace_context = callback ? context : NULL;
    return 0;
}
/* Firmware-specific Processmgr exports must never be mandatory static imports. */
static SceClass *process_class(void)
{
    const uint32_t libraries[] = {0x7A69DE86, 0xEB1F8EF7};
    const uint32_t functions[] = {0xC6820972, 0x98AE4BC8};
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t address = 0;
        if (module_get_export_func(KERNEL_PID, "SceProcessmgr", libraries[i], functions[i],
                                   &address) >= 0 &&
            address)
            return ((SceClass * (*)(void)) address)();
    }
    return NULL;
}
static uintptr_t block_type_function(void)
{
    const uint32_t libraries[] = {0x63A519E5, 0x02451F0F};
    const uint32_t functions[] = {0x289BE3EC, 0xD44FE44B};
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t address = 0;
        if (module_get_export_func(KERNEL_PID, "SceSysmem", libraries[i], functions[i], &address) >=
                0 &&
            address)
            return address;
    }
    return 0;
}
static bool same(const VcoIdentity *a, const VcoIdentity *b)
{
    return a && b && a->pid == b->pid && a->instance == b->instance &&
           !memcmp(a->title_id, b->title_id, sizeof(a->title_id));
}
static int poison(VcoVitaMemory *m, int result)
{
    m->poisoned = true;
    m->cleanup_error = result;
    return -1;
}
void vco_vita_memory_init(VcoVitaMemory *m, VcoVitaAuthority authority, void *context)
{
    memset(m, 0, sizeof(*m));
    m->authority = authority;
    m->authority_context = context;
    m->block = -1;
}
int vco_vita_memory_bind(VcoVitaMemory *m, int32_t pid, const char title_id[10], VcoIdentity *out)
{
    if (!m || !title_id || !out || !m->authority || m->bound || m->locked || m->poisoned ||
        m->sequence == UINT64_MAX || pid == KERNEL_PID)
        return -1;
    VcoIdentity candidate = {.pid = pid, .instance = m->sequence + 1};
    memcpy(candidate.title_id, title_id, sizeof(candidate.title_id));
    if (!vco_identity_valid(&candidate))
        return -1;
    SceClass *cls = process_class();
    m->get_block_type = (int (*)(int32_t, unsigned *))block_type_function();
    if (!m->get_block_type)
        return -1;
    SceObjectBase *object = NULL;
    if (!cls || ksceGUIDReferObjectWithClass(pid, cls, &object) < 0)
        return -1;
    /* Consume the generation even on rejected authority: never reuse a token. */
    ++m->sequence;
    if (!object || m->authority(m->authority_context, &candidate, false)) {
        int result = ksceGUIDReleaseObject(pid);
        return result < 0 ? poison(m, result) : -1;
    }
    m->identity = candidate;
    m->process_class = cls;
    m->process_object = object;
    m->bound = true;
    *out = candidate;
    return 0;
}
static int validate(void *context, const VcoIdentity *identity, bool foreground)
{
    VcoVitaMemory *m = context;
    if (!m || m->poisoned || !m->authority)
        return -1;
    if (!m->bound || !same(identity, &m->identity))
        return VCO_ACCESS_SESSION;
    SceObjectBase *current = NULL;
    int referred = ksceGUIDReferObjectWithClass(identity->pid, m->process_class, &current);
    if (referred < 0)
        return failure(m, VCO_MEMORY_TRACE_PROCESS_REFER_FAILURE, referred, (uint32_t)identity->pid,
                       foreground);
    bool match = current && current == m->process_object;
    int result = ksceGUIDReleaseObject(identity->pid);
    if (result < 0) {
        failure(m, VCO_MEMORY_TRACE_PROCESS_RELEASE_FAILURE, result, (uint32_t)identity->pid,
                foreground);
        return poison(m, result);
    }
    if (!match) {
        failure(m, VCO_MEMORY_TRACE_PROCESS_CHANGED, -1, (uint32_t)identity->pid, foreground);
        return VCO_ACCESS_SESSION;
    }
    result = m->authority(m->authority_context, identity, foreground);
    if (result)
        failure(m, VCO_MEMORY_TRACE_AUTHORITY_FAILURE, result, (uint32_t)identity->pid, foreground);
    return result ? VCO_ACCESS_SESSION : 0;
}
static bool range_valid(uint32_t address, size_t size)
{
    return (size == 1 || size == 2 || size == 4) && address >= 0x40000000u &&
           (uint64_t)address + size <= UINT64_C(0x100000000);
}
static int unlock(void *context, const VcoIdentity *identity, uint32_t address, size_t size)
{
    VcoVitaMemory *m = context;
    /* Cleanup must work after authority revocation, using the retained binding. */
    if (!m || !m->locked || !same(identity, &m->identity) || address != m->address ||
        size != m->length)
        return -1;
    int result =
        ksceKernelUnlockRangeProc(m->identity.pid, (void *)(uintptr_t)m->address, m->length);
    /* Failure may have consumed the reference: don't retry an ambiguous release. */
    m->locked = false;
    m->block = -1;
    if (result < 0) {
        failure(m, VCO_MEMORY_TRACE_UNLOCK_FAILURE, result, address, (uint32_t)size);
        return poison(m, result);
    }
    return 0;
}
static int lock(void *context, const VcoIdentity *identity, uint32_t address, size_t size)
{
    VcoVitaMemory *m = context;
    if (!m || m->locked || !range_valid(address, size))
        return -1;
    int checked = validate(m, identity, true);
    if (checked)
        return checked;
    int result = ksceKernelLockRangeProc(identity->pid, (void *)(uintptr_t)address, size);
    if (result < 0)
        return failure(m, VCO_MEMORY_TRACE_LOCK_FAILURE, result, address, (uint32_t)size);
    m->locked = true;
    m->address = address;
    m->length = (uint32_t)size;
    m->block = vco_vita_find_memblock(identity->pid, address, size);
    if (m->block < 0)
        failure(m, VCO_MEMORY_TRACE_FIND_FAILURE, m->block, address, (uint32_t)size);
    checked = m->block < 0 ? -1 : validate(m, identity, true);
    if (checked) {
        return unlock(m, identity, address, size) ? -1 : checked;
    }
    return 0;
}
/* Exact known types, not guessed permission bits; unknown/device/shared types
 * are rejected until their behavior is covered by native validation. */
static uint32_t permissions(unsigned type)
{
    switch (type) {
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R:
        return VCO_MEM_READ;
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RX:
        return VCO_MEM_READ | VCO_MEM_EXECUTE;
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW:
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW:
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW:
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW:
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW:
        return VCO_MEM_READ | VCO_MEM_WRITE;
    default:
        return 0;
    }
}
static int inspect(void *context, const VcoIdentity *identity, uint32_t address, size_t size,
                   VcoMemoryRegion *out)
{
    VcoVitaMemory *m = context;
    if (!m || !out || !m->locked || address != m->address || size != m->length)
        return -1;
    int checked = validate(m, identity, true);
    if (checked)
        return checked;
    SceUID block = vco_vita_find_memblock(identity->pid, address, size);
    unsigned type = 0;
    if (block < 0)
        return failure(m, VCO_MEMORY_TRACE_FIND_FAILURE, block, address, (uint32_t)size);
    if (block != m->block)
        return failure(m, VCO_MEMORY_TRACE_BLOCK_CHANGED, block, address, (uint32_t)m->block);
    int result = m->get_block_type(block, &type);
    if (result < 0)
        return failure(m, VCO_MEMORY_TRACE_TYPE_FAILURE, result, address, (uint32_t)block);
    uint32_t perms = permissions(type);
    if (!perms)
        return failure(m, VCO_MEMORY_TRACE_TYPE_DENIED, -1, address, type);
    /* Export only the locked subrange, not an unverified whole memblock extent. */
    *out = (VcoMemoryRegion){address, (uint32_t)size, perms, identity->instance};
    return 0;
}
static int read_data(void *context, const VcoIdentity *identity, uint32_t address, uint8_t *out,
                     size_t size)
{
    VcoMemoryRegion region;
    if (!out)
        return -1;
    int checked = inspect(context, identity, address, size, &region);
    if (checked)
        return checked;
    if (!(region.permissions & VCO_MEM_READ))
        return -1;
    uint8_t staged[4];
    int result =
        ksceKernelCopyFromUserProc(identity->pid, staged, (void *)(uintptr_t)address, size);
    if (result < 0)
        return failure(context, VCO_MEMORY_TRACE_COPY_READ_FAILURE, result, address,
                       (uint32_t)size);
    checked = validate(context, identity, true);
    if (checked)
        return checked;
    memcpy(out, staged, size);
    return 0;
}
static int write_data(void *context, const VcoIdentity *identity, uint32_t address,
                      const uint8_t *in, size_t size)
{
    VcoMemoryRegion region;
    if (!in)
        return -1;
    int checked = inspect(context, identity, address, size, &region);
    if (checked)
        return checked;
    if (!(region.permissions & VCO_MEM_WRITE) || (region.permissions & VCO_MEM_EXECUTE))
        return -1;
    /* Keep DACR permission checks. No TextDomain/RX copy or temporary remapping. */
    int result = ksceKernelCopyToUserProc(identity->pid, (void *)(uintptr_t)address, in, size);
    if (result < 0)
        return failure(context, VCO_MEMORY_TRACE_COPY_WRITE_FAILURE, result, address,
                       (uint32_t)size);
    return validate(context, identity, true);
}
int vco_vita_memory_unbind(VcoVitaMemory *m)
{
    if (!m || m->locked)
        return -1;
    if (!m->bound)
        return m->poisoned ? -1 : 0;
    int result = ksceGUIDReleaseObject(m->identity.pid);
    m->bound = false;
    m->process_object = NULL;
    if (result < 0)
        return poison(m, result);
    return m->poisoned ? -1 : 0;
}
VcoMemoryTransport vco_vita_memory_transport(VcoVitaMemory *m)
{
    return (VcoMemoryTransport){m, lock, inspect, read_data, write_data, unlock, validate};
}
