#ifndef VCO_EXECUTION_SERVICE_H
#define VCO_EXECUTION_SERVICE_H
#include "../execution/control.h"
#include "../catalog/catalog.h"
typedef enum {
    VCO_EXECUTION_LOADING,
    VCO_EXECUTION_READY,
    VCO_EXECUTION_BACKGROUND,
    VCO_EXECUTION_NO_GAME,
    VCO_EXECUTION_ERROR,
    VCO_EXECUTION_CONFLICT,
    VCO_EXECUTION_UNAVAILABLE
} VcoExecutionServiceState;
/* User-side snapshot only; not a wire structure. revision tracks publication,
 * catalog_revision matches the exact displayed catalog. No optimistic ON. */
typedef struct {
    uint64_t session, list_revision;
    uint32_t revision, catalog_revision, state, error;
    unsigned count, enabled_count;
    bool known[VCO_ENGINE_ENTRIES], queued[VCO_ENGINE_ENTRIES];
    VcoControlRow rows[VCO_ENGINE_ENTRIES];
} VcoExecutionView;
#ifdef __cplusplus
extern "C" {
#endif
/* Starts the independent observer and control workers. The control worker also
 * saves at most one native trace batch per 100 ms to the bounded native.log.
 * Diagnostics never change execution state; a save failure disables collection
 * until this user module is restarted. */
int vco_execution_service_start(void);
/* Nonblocking try-lock: 1 fresh snapshot; 0 unchanged/busy. */
int vco_execution_service_copy(VcoExecutionView *out);
/* UI supplies its confirmed binding. 1 queued; 0 busy; -1 rejected. */
int vco_execution_service_set(uint64_t session, uint64_t list_revision,
                              unsigned entry, bool enabled);
/* Catalog worker only: synchronous bounded copy into owned staging storage;
 * never borrows workspace after return. NULL work withdraws the displayed list.
 * Reload uses a fresh catalog revision; every accepted upload starts OFF. */
int vco_execution_service_catalog(const VcoCatalog *, const VcoCatalogWorkspace *);
#ifdef __cplusplus
}
#endif
#endif
