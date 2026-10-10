#ifndef VCO_VITA_CLOSE_SERVICE_H
#define VCO_VITA_CLOSE_SERVICE_H
#include "control_worker.h"
/* Resident-kernel-only, deliberately absent from syscall/bridge exports.
 * A future verified native boundary producer must hold the original process
 * and module references plus mapping protection independently, match them to
 * these tags, and establish its wait contract. These helpers do not do that. */
typedef struct {
    VcoEngineCloseKey key;
    uint64_t module_snapshot;
} VcoVitaCloseTarget;
/* Nonblocking copy of worker-published metadata; no Shell call or SDK lookup.
 * Stale metadata is rejected by the worker before it performs cleanup. */
VcoCloseStatus vco_service_close_target(VcoVitaCloseTarget *);
VcoCloseStatus vco_service_close_submit(uint64_t requester, const VcoEngineCloseKey *,
                                        uint64_t *ticket);
VcoCloseStatus vco_service_close_receive(uint64_t requester, uint64_t ticket, VcoVitaCloseResult *);
VcoCloseStatus vco_service_close_release(uint64_t requester, uint64_t ticket);
#endif
