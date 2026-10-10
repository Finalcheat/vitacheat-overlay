#ifndef VCO_GAME_VITA_ALLOCATOR_H
#define VCO_GAME_VITA_ALLOCATOR_H
#include "executor.h"
#define VCO_GAME_BLOCKS 3u /* payload, current list, candidate replacement */
#define VCO_GAME_ALLOCATION_MAX                                                                    \
    (VCO_ENGINE_ENTRIES * sizeof(VcoCheatEntry) +                                                  \
     VCO_UPLOAD_CODES * (sizeof(VcoCode) + sizeof(VcoCheatOp) + sizeof(VcoPatch)) + 64u)
typedef struct {
    int32_t uid;
    void *base;
    uint32_t bytes;
    bool release_failed;
} VcoGameBlock;
/* Owned by the game worker; initialize once. No libc heap and no kernel-held
 * references. A failed native free is not retried or silently reused. */
typedef struct {
    VcoGameBlock blocks[VCO_GAME_BLOCKS];
    bool initialized, poisoned;
    int native_error;
} VcoGamePool;
VcoError vco_game_pool_init(VcoGamePool *);
VcoAgentAllocator vco_game_pool_allocator(VcoGamePool *);
VcoError vco_game_pool_dispose(VcoGamePool *);
#endif
