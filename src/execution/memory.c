#include "memory.h"
#include <string.h>
static int alive(void *ctx, uint64_t token)
{
    VcoMemoryAdapter *a = ctx;
    return a && vco_session_matches(a->session, token, true) && a->transport.validate &&
           a->transport.validate(a->transport.context, &a->session->identity, true) == 0;
}
static int access_memory(VcoMemoryAdapter *a, uint64_t token, uint32_t address, uint8_t *output,
                         const uint8_t *input, size_t size)
{
    /* Vita user virtual address space begins at 0x40000000. Range and permission
     * checks do not authorize a PID; validated instance authority is required too. */
    bool writing = input != NULL;
    if (!a || (!output && !input) || (size != 1 && size != 2 && size != 4) ||
        address < 0x40000000u || (uint64_t)address + size > UINT64_C(0x100000000))
        return VCO_MEMORY_ARGUMENT;
    if (!alive(a, token))
        return VCO_MEMORY_AUTHORITY;
    VcoMemoryTransport *t = &a->transport;
    if (!t->lock || !t->inspect || !t->unlock || (writing ? !t->write : !t->read))
        return VCO_MEMORY_CAPABILITY;
    VcoIdentity identity = a->session->identity;
    int locked = t->lock(t->context, &identity, address, size);
    if (locked)
        return locked == VCO_ACCESS_SESSION ? VCO_MEMORY_AUTHORITY : VCO_MEMORY_LOCK;
    VcoMemoryRegion region = {0};
    int result;
    uint8_t staged[4] = {0};
    if (!alive(a, token))
        result = VCO_MEMORY_AUTHORITY;
    else {
        int inspected = t->inspect(t->context, &identity, address, size, &region);
        result = inspected == VCO_ACCESS_SESSION ? VCO_MEMORY_AUTHORITY
                 : inspected                     ? VCO_MEMORY_INSPECT
                                                 : 0;
    }
    if (!result &&
        (region.instance != identity.instance || region.base < 0x40000000u || !region.size ||
         (uint64_t)region.base + region.size > UINT64_C(0x100000000) || address < region.base ||
         (uint64_t)address + size > (uint64_t)region.base + region.size))
        result = VCO_MEMORY_REGION;
    else if (!result && (!(region.permissions & VCO_MEM_READ) ||
                         (writing && (!(region.permissions & VCO_MEM_WRITE) ||
                                      (region.permissions & VCO_MEM_EXECUTE)))))
        result = VCO_MEMORY_PERMISSION;
    else if (!result && !alive(a, token))
        result = VCO_MEMORY_AUTHORITY;
    else if (!result) {
        int copied = writing ? t->write(t->context, &identity, address, input, size)
                             : t->read(t->context, &identity, address, staged, size);
        result = copied == VCO_ACCESS_SESSION ? VCO_MEMORY_AUTHORITY : copied ? VCO_MEMORY_COPY : 0;
    }
    if (t->unlock(t->context, &identity, address, size))
        result = VCO_MEMORY_UNLOCK;
    if (!result && !alive(a, token))
        result = VCO_MEMORY_AUTHORITY;
    if (!result && !writing)
        memcpy(output, staged, size);
    return result;
}
static int read_memory(void *ctx, uint64_t token, uint32_t address, uint8_t *out, size_t size)
{
    return access_memory(ctx, token, address, out, NULL, size);
}
static int write_memory(void *ctx, uint64_t token, uint32_t address, const uint8_t *in, size_t size)
{
    return access_memory(ctx, token, address, NULL, in, size);
}
VcoCheatMemory vco_memory_callbacks(VcoMemoryAdapter *adapter)
{
    VcoCheatMemory memory = {0};
    memory.context = adapter;
    memory.alive = alive;
    memory.read = read_memory;
    memory.write = write_memory;
    return memory;
}
