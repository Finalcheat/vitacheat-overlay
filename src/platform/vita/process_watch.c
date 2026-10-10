#include "process_watch.h"
#include <string.h>

static void increment(uint32_t *value)
{
    if (*value != UINT32_MAX)
        ++*value;
}
static void revoke(VcoVitaProcessWatch *watch)
{
    if (vco_session_observe(watch->session, VCO_OBSERVATION_UNKNOWN, NULL))
        increment(&watch->revocations);
}
void vco_vita_process_watch_init(VcoVitaProcessWatch *watch, VcoSession *session)
{
    if (!watch)
        return;
    memset(watch, 0, sizeof(*watch));
    watch->session = session;
    revoke(watch);
}
void vco_vita_process_watch_update(void *context, uint64_t now_us)
{
    (void)now_us;
    VcoVitaProcessWatch *watch = context;
    if (!watch || !watch->session)
        return;
    VcoProcessEventsState before = {0}, after = {0};
    vco_process_events_state(&before);
    uint32_t generation = vco_process_events_invalidation();
    bool uncertain = !watch->sampled || generation != watch->invalidation ||
                     before.phase != VCO_PROCESS_EVENTS_ACTIVE;
    if (generation & VCO_PROCESS_EVENTS_LOST)
        watch->lost = true;
    /* Revoke before draining so no code on this worker sees an old runnable
     * state during potentially contended diagnostic queue operations. */
    if (uncertain || watch->lost)
        revoke(watch);
    bool observed = false, failed = false;
    for (unsigned i = 0; i < VCO_PROCESS_EVENTS_CAPACITY; ++i) {
        VcoProcessEvent event;
        VcoProcessEventsPoll result = vco_process_events_poll(&event);
        if (result == VCO_PROCESS_EVENTS_EMPTY)
            break;
        if (result != VCO_PROCESS_EVENTS_OK) {
            uncertain = true;
            failed = true;
            break;
        }
        observed = true;
        uncertain = true;
        watch->last_event = event;
        increment(&watch->records);
        if (watch->session->present && event.pid == watch->session->identity.pid &&
            (event.kind == VCO_PROCESS_EVENT_EXIT || event.kind == VCO_PROCESS_EVENT_KILL))
            watch->terminal_seen = true;
    }
    vco_process_events_state(&after);
    uint32_t final_generation = vco_process_events_invalidation();
    if (final_generation & VCO_PROCESS_EVENTS_LOST)
        watch->lost = true;
    if (generation != final_generation || after.phase != VCO_PROCESS_EVENTS_ACTIVE)
        uncertain = true;
    if (uncertain || watch->lost)
        revoke(watch);
    watch->invalidation = final_generation;
    watch->sampled = !observed && !failed && !watch->lost &&
                     before.phase == VCO_PROCESS_EVENTS_ACTIVE &&
                     after.phase == VCO_PROCESS_EVENTS_ACTIVE && generation == final_generation;
    /* An event-containing drain can become a baseline on the next empty,
     * stable update. Neither update restores the session's runnable flag. */
}
bool vco_vita_process_watch_unchanged(const VcoVitaProcessWatch *watch)
{
    if (!watch || !watch->session || !watch->sampled || watch->lost)
        return false;
    VcoProcessEventsState state = {0};
    vco_process_events_state(&state);
    uint32_t generation = vco_process_events_invalidation();
    return state.phase == VCO_PROCESS_EVENTS_ACTIVE && !(generation & VCO_PROCESS_EVENTS_LOST) &&
           generation == watch->invalidation;
}
