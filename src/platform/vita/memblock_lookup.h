#ifndef VCO_VITA_MEMBLOCK_LOOKUP_H
#define VCO_VITA_MEMBLOCK_LOOKUP_H
#include <stddef.h>
#include <stdint.h>
#include <psp2kern/kernel/sysmem.h>

/* FindProcMemBlockByAddr's nonzero size selects an exact allocation extent,
 * not an interior range. Use point lookups and require one block for the
 * entire 1/2/4-byte operation. Preserve raw lookup errors from either end.
 * Callers retain their full-width range locks and permission/object checks;
 * this observation alone does not synchronize mapping destruction. */
static inline SceUID vco_vita_find_memblock(SceUID pid, uint32_t address, size_t size)
{
    if ((size != 1 && size != 2 && size != 4) || address < UINT32_C(0x40000000) ||
        (uint64_t)address + size > UINT64_C(0x100000000))
        return -1;
    SceUID first = ksceKernelFindProcMemBlockByAddr(pid, (const void *)(uintptr_t)address, 0);
    if (first < 0 || size == 1)
        return first;
    uint32_t last_address = address + (uint32_t)size - 1;
    SceUID last = ksceKernelFindProcMemBlockByAddr(pid, (const void *)(uintptr_t)last_address, 0);
    return last < 0 ? last : last == first ? first : -1;
}
#endif
