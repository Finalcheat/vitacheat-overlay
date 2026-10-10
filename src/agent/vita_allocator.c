#include "vita_allocator.h"
#include <psp2/kernel/sysmem.h>
#include <string.h>
VcoError vco_game_pool_init(VcoGamePool *p)
{
    if (!p || p->initialized)
        return VCO_E_ARGUMENT;
    memset(p, 0, sizeof(*p));
    for (unsigned i = 0; i < VCO_GAME_BLOCKS; ++i)
        p->blocks[i].uid = -1;
    p->initialized = true;
    return VCO_OK;
}
static void release_block(VcoGamePool *p, VcoGameBlock *b)
{
    if (b->uid < 0 || b->release_failed)
        return;
    int error = sceKernelFreeMemBlock(b->uid);
    if (error < 0) {
        b->release_failed = p->poisoned = true;
        p->native_error = error;
    } else
        *b = (VcoGameBlock){.uid = -1};
}
static void *allocate(void *context, size_t bytes)
{
    VcoGamePool *p = context;
    if (!p || !p->initialized || p->poisoned || !bytes || bytes > VCO_GAME_ALLOCATION_MAX)
        return NULL;
    VcoGameBlock *b = NULL;
    for (unsigned i = 0; i < VCO_GAME_BLOCKS; ++i)
        if (p->blocks[i].uid < 0) {
            b = &p->blocks[i];
            break;
        }
    if (!b)
        return NULL;
    uint32_t rounded = (uint32_t)((bytes + 4095u) & ~(size_t)4095u);
    int uid =
        sceKernelAllocMemBlock("vco_game_data", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, rounded, NULL);
    if (uid < 0) {
        p->native_error = uid;
        return NULL;
    }
    *b = (VcoGameBlock){.uid = uid, .bytes = rounded};
    int error = sceKernelGetMemBlockBase(uid, &b->base);
    if (error < 0 || !b->base || ((uintptr_t)b->base & 15u)) {
        p->native_error = error < 0 ? error : -1;
        release_block(p, b);
        return NULL;
    }
    return b->base;
}
static void release(void *context, void *base)
{
    VcoGamePool *p = context;
    if (!p || !p->initialized || !base)
        return;
    for (unsigned i = 0; i < VCO_GAME_BLOCKS; ++i)
        if (p->blocks[i].uid >= 0 && p->blocks[i].base == base) {
            release_block(p, &p->blocks[i]);
            return;
        }
    p->poisoned = true;
    p->native_error = -1;
}
VcoAgentAllocator vco_game_pool_allocator(VcoGamePool *p)
{
    return (VcoAgentAllocator){p, allocate, release};
}
VcoError vco_game_pool_dispose(VcoGamePool *p)
{
    if (!p || !p->initialized)
        return VCO_E_ARGUMENT;
    for (unsigned i = 0; i < VCO_GAME_BLOCKS; ++i)
        release_block(p, &p->blocks[i]);
    return p->poisoned ? VCO_E_IO : VCO_OK;
}
