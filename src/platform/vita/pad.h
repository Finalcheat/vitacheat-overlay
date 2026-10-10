#ifndef VCO_VITA_PAD_H
#define VCO_VITA_PAD_H
#include "../../execution/memory.h"
/* Trusted capture snapshot: 0 clear, 1 overlay capture/drain, other values are
 * errors. Write epoch only on success. Epoch increments on capture transitions;
 * a lease expiring from active to clear may retain its epoch. Capture snapshots
 * must come from the kernel capture authority, never UI-supplied state. */
typedef int (*VcoVitaPadSuppression)(void *context, uint32_t *epoch);
/* Single execution-worker owner. No controller mode changes, hooks, allocation,
 * blocking reads, or user pointers. Adapter and callback context outlive this
 * object. New sessions, capture changes, and failures require release followed
 * by a fresh digital press before conditions can see input again. */
typedef struct {
    const VcoMemoryAdapter *data;
    VcoVitaPadSuppression suppression;
    void *suppression_context;
    uint64_t token, release_stamp;
    uint32_t epoch;
    uint8_t ports[5];
    bool epoch_known, topology_known, poisoned, armed, released;
} VcoVitaPad;
bool vco_vita_pad_init(VcoVitaPad *, const VcoMemoryAdapter *, VcoVitaPadSuppression, void *);
/* VcoCheatMemory.pad signature. 0 publishes a complete device/buttons pair;
 * VCO_PAD_SUPPRESSED publishes neither; any other nonzero result is an error.
 * Matches original Z06's ordinary positive peek at logical port 0. Controller
 * selection belongs to SceCtrl; no Ext layout or physical-port remapping.
 * Device reports logical port 0's SDK type, not a guessed physical identity.
 * Known external/virtual types are accepted. A topology change requires release
 * and a fresh press, just like a capture transition. Unknown types fail closed.
 * An epoch rollback/wrap poisons the object until explicit reinitialization. */
int vco_vita_pad_read(void *, uint64_t token, uint32_t *device, uint32_t *buttons);
#endif
