#ifndef VCO_VITA_PROCESS_WATCH_H
#define VCO_VITA_PROCESS_WATCH_H
#include "process_events.h"
#include "../../execution/session.h"

/* All APIs, including unchanged(), run on the same worker; fields in this
 * object are not atomic. Single consumer of the singleton event stream. No SDK callbacks touch
 * this object or the session; their atomic invalidation is checked by unchanged.
 * This is revocation only. PID-only event evidence cannot prove which retained
 * process instance exited and must not declare a replacement instance absent. */
typedef struct {
    VcoSession *session;
    VcoProcessEvent last_event;
    uint32_t invalidation, records, revocations;
    bool sampled, lost, terminal_seen;
} VcoVitaProcessWatch;
void vco_vita_process_watch_init(VcoVitaProcessWatch *, VcoSession *);
/* VcoVitaControlUpdate-compatible; call before mailbox_pump/engine_step.
 * Drains at most CAPACITY records and revokes runnable on any source change,
 * contention, lost evidence, or inactive source. Never grants foreground or
 * clears identity. terminal_seen is diagnostic only and sticky for this watch. */
void vco_vita_process_watch_update(void *, uint64_t now_us);
/* Cheap supplemental check at an authority boundary, including before worker
 * update sees a new event. True means only that the evidence hasn't changed
 * since the last successful drain, NOT permission to access the game. A callback
 * can have invalidated before enqueueing: this is not a callback-quiescence or
 * queue-completeness proof. */
bool vco_vita_process_watch_unchanged(const VcoVitaProcessWatch *);
#endif
