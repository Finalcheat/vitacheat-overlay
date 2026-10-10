#ifndef VCO_GAME_VITA_MEMORY_H
#define VCO_GAME_VITA_MEMORY_H
#include "native_access.h"
#include "executor.h"
/* Zero initialize once, then a single game worker owns this provider. Never
 * reinitialize an initialized provider. Module descriptors are observations,
 * not lifetime pins. Live-game module boundary serialization is separate. */
typedef struct {
    int32_t key, native_uid;
    uint32_t address;
    int16_t owner;
    uint8_t width, state;
} VcoGameNativePatch;
typedef struct {
    VcoAgentBinding binding;
    VcoGameModuleList list;
    VcoGameModuleInfo modules[VCO_GAME_MODULE_LIMIT];
    VcoGameNativePatch patches[VCO_ENGINE_CLAIMS];
    int32_t sequence;
    unsigned owned;
    unsigned diagnostics;
    int snapshot_error;
    int restore_cause_result;
    unsigned restore_cause_event;
    uint32_t restore_cause_subject, restore_cause_detail;
    uint64_t release_stamp;
    uint32_t capture_epoch, device;
    bool initialized, mapped, blocked, pad_known, released, armed;
    bool inject_logged, release_logged;
    bool restore_failure_logged;
} VcoGameNativeMemory;
VcoError vco_game_native_memory_init(VcoGameNativeMemory *, const VcoAgentBinding *);
VcoCheatMemory vco_game_native_memory_callbacks(VcoGameNativeMemory *);
/* VcoEngineIdlePrepare: only at an explicit ON boundary with every row OFF and
 * no native patches. This is not a periodic automatic module remapping. */
void vco_game_native_memory_prepare(void *, uint64_t session);
/* Releases private/unpublished receipts too. Consumed failed native UIDs are
 * never retried; blocked restore prevents explicit unloading. Process exit
 * does not call this; taiHEN performs target-PID cleanup. */
VcoError vco_game_native_memory_finalize(void *);
#endif
